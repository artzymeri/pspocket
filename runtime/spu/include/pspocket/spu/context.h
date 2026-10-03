/*
 * SPU thread context, shared by the runtime (C++20), the interpreter and lifted code (C11).
 *
 * Representation (docs/decisions.md D-004 and the SPU entries):
 *
 * - 128 GPRs of 128 bits in the full-reverse lane order: host byte j holds guest byte 15 - j, so guest element i of an
 *   N-element vector is host lane N-1-i. The SPU preferred slot (guest word 0) is host word 3, the preferred halfword
 *   (guest halfword 1) is host halfword 6, the preferred byte (guest byte 3) is host byte 12.
 * - pc is a local-store address (multiple of 4, below 256 KB). ls points to the 256 KB local store (16-byte
 *   aligned). LS addresses wrap at 256 KB; quadword accesses mask to 16-byte alignment.
 * - FPSCR keeps the architected bits only (RPCS3 SPU_FPSCR write masks). Arithmetic does not set exception flags;
 *   see pspocket/spu/fp.h for the floating-point model.
 * - Channel, MFC, event and decrementer state is plain data (ps_spu_channels) so tests can compare it; the runtime
 *   protects the parts other threads touch with the lock in `host` (runtime-private).
 * - stop_reason / stop_code / exception say why execution returned to the run loop (PS_SPU_STOP_*).
 */
#ifndef PSPOCKET_SPU_CONTEXT_H
#define PSPOCKET_SPU_CONTEXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define PS_SPU_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
extern "C" {
#else
#define PS_SPU_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/* ---- local store ---- */
#define PS_SPU_LS_SIZE 0x40000u
#define PS_SPU_LS_MASK 0x3FFFFu
#define PS_SPU_PC_MASK 0x3FFFCu /* branch targets and the pc */
#define PS_SPU_QW_MASK 0x3FFF0u /* quadword loads and stores */

/* ---- channels (RPCS3 Emu/Cell/SPUThread.h) ---- */
enum {
    PS_SPU_RdEventStat = 0,
    PS_SPU_WrEventMask = 1,
    PS_SPU_WrEventAck = 2,
    PS_SPU_RdSigNotify1 = 3,
    PS_SPU_RdSigNotify2 = 4,
    PS_SPU_WrDec = 7,
    PS_SPU_RdDec = 8,
    PS_MFC_WrMSSyncReq = 9,
    PS_SPU_RdEventMask = 11,
    PS_MFC_RdTagMask = 12,
    PS_SPU_RdMachStat = 13,
    PS_SPU_WrSRR0 = 14,
    PS_SPU_RdSRR0 = 15,
    PS_MFC_LSA = 16,
    PS_MFC_EAH = 17,
    PS_MFC_EAL = 18,
    PS_MFC_Size = 19,
    PS_MFC_TagID = 20,
    PS_MFC_Cmd = 21,
    PS_MFC_WrTagMask = 22,
    PS_MFC_WrTagUpdate = 23,
    PS_MFC_RdTagStat = 24,
    PS_MFC_RdListStallStat = 25,
    PS_MFC_WrListStallAck = 26,
    PS_MFC_RdAtomicStat = 27,
    PS_SPU_WrOutMbox = 28,
    PS_SPU_RdInMbox = 29,
    PS_SPU_WrOutIntrMbox = 30,
    PS_SPU_Set_Bkmk_Tag = 69,
    PS_SPU_PM_Start_Ev = 70,
    PS_SPU_PM_Stop_Ev = 71,
};

/* ---- events (SPU_RdEventStat bits) ---- */
enum {
    PS_SPU_EVENT_MS = 0x1000, /* multisource synchronization */
    PS_SPU_EVENT_A = 0x800,   /* privileged attention */
    PS_SPU_EVENT_LR = 0x400,  /* lock-line reservation lost */
    PS_SPU_EVENT_S1 = 0x200,  /* signal notification 1 */
    PS_SPU_EVENT_S2 = 0x100,  /* signal notification 2 */
    PS_SPU_EVENT_LE = 0x80,   /* outbound mailbox available */
    PS_SPU_EVENT_ME = 0x40,   /* outbound interrupt mailbox available */
    PS_SPU_EVENT_TM = 0x20,   /* decrementer */
    PS_SPU_EVENT_MB = 0x10,   /* inbound mailbox */
    PS_SPU_EVENT_QV = 0x8,    /* MFC command queue vacancy */
    PS_SPU_EVENT_SN = 0x2,    /* list stall-and-notify */
    PS_SPU_EVENT_TG = 0x1,    /* tag group status update */
    PS_SPU_EVENT_ALL = 0x1FFF,
};

/* ---- MFC commands (RPCS3 Emu/Cell/MFC.h) ---- */
enum {
    PS_MFC_PUT = 0x20, PS_MFC_PUTB = 0x21, PS_MFC_PUTF = 0x22,
    PS_MFC_PUTR = 0x30, PS_MFC_PUTRB = 0x31, PS_MFC_PUTRF = 0x32,
    PS_MFC_GET = 0x40, PS_MFC_GETB = 0x41, PS_MFC_GETF = 0x42,
    PS_MFC_PUTL = 0x24, PS_MFC_PUTLB = 0x25, PS_MFC_PUTLF = 0x26,
    PS_MFC_PUTRL = 0x34, PS_MFC_PUTRLB = 0x35, PS_MFC_PUTRLF = 0x36,
    PS_MFC_GETL = 0x44, PS_MFC_GETLB = 0x45, PS_MFC_GETLF = 0x46,
    PS_MFC_GETLLAR = 0xD0,
    PS_MFC_PUTLLC = 0xB4,
    PS_MFC_PUTLLUC = 0xB0,
    PS_MFC_PUTQLLUC = 0xB8,
    PS_MFC_SNDSIG = 0xA0, PS_MFC_SNDSIGB = 0xA1, PS_MFC_SNDSIGF = 0xA2,
    PS_MFC_BARRIER = 0xC0,
    PS_MFC_EIEIO = 0xC8,
    PS_MFC_SYNC = 0xCC,
    PS_MFC_SDCRT = 0x80, PS_MFC_SDCRTST = 0x81, PS_MFC_SDCRZ = 0x89, PS_MFC_SDCRS = 0x8D, PS_MFC_SDCRF = 0x8F,

    PS_MFC_BARRIER_MASK = 0x01,
    PS_MFC_FENCE_MASK = 0x02,
    PS_MFC_LIST_MASK = 0x04,
};

/* MFC_RdAtomicStat values */
enum { PS_MFC_PUTLLC_SUCCESS = 0, PS_MFC_PUTLLC_FAILURE = 1, PS_MFC_PUTLLUC_SUCCESS = 2, PS_MFC_GETLLAR_SUCCESS = 4 };
/* MFC_WrTagUpdate requests */
enum { PS_MFC_TAG_UPDATE_IMMEDIATE = 0, PS_MFC_TAG_UPDATE_ANY = 1, PS_MFC_TAG_UPDATE_ALL = 2 };

/* DMA alignment and size limits */
#define PS_MFC_MAX_SIZE 0x4000u /* 16 KB per transfer and per list */

/* SPU thread group MMIO window (RPCS3 SPUThread.h): LS and signal registers of the other threads of the group. */
#define PS_SPU_RAW_BASE 0xE0000000u
#define PS_SPU_THREAD_BASE 0xF0000000u
#define PS_SPU_THREAD_OFFSET 0x00100000u
#define PS_SPU_THREAD_SNR1 0x0005400Cu
#define PS_SPU_THREAD_SNR2 0x0005C00Cu

/* ---- why execution returned to the run loop (ctx->stop_reason) ---- */
enum {
    PS_SPU_STOP_NONE = 0,      /* running */
    PS_SPU_STOP_SIGNAL = 1,    /* stop / stopd: stop_code = signal type (14 bits), pc = next instruction */
    PS_SPU_STOP_HALT = 2,      /* heq/hgt/hlgt condition true: pc = the halting instruction */
    PS_SPU_STOP_EXCEPTION = 3, /* exception = PS_SPU_EXC_*, stop_code = detail, pc = faulting instruction */
    PS_SPU_STOP_REQUESTED = 4, /* external stop request (thread group stop/terminate): pc = next instruction */
    PS_SPU_STOP_BLOCKED = 5,   /* a channel would block and the context has no host (single-thread use) */
    PS_SPU_STOP_LIMIT = 6,     /* interpreter step limit reached */
};

enum {
    PS_SPU_EXC_NONE = 0,
    PS_SPU_EXC_ILLEGAL = 1,       /* undecodable or unsupported instruction; stop_code = instruction word */
    PS_SPU_EXC_DMA_ALIGNMENT = 2, /* size/alignment rules of a DMA command; stop_code = command opcode */
    PS_SPU_EXC_DMA_COMMAND = 3,   /* invalid MFC command or size; stop_code = command opcode */
    PS_SPU_EXC_DMA_ADDRESS = 4,   /* effective address the runtime cannot serve (raw SPU MMIO, bad group MMIO) */
    PS_SPU_EXC_CHANNEL = 5,       /* read or write of an invalid channel; stop_code = channel */
    PS_SPU_EXC_DEADLOCK = 6,      /* channel read that can never complete (no producer); stop_code = channel */
    PS_SPU_EXC_INVALID_FORM = 7,  /* invalid instruction form (branch with both E and D) */
};

/* ---- SPU status register (raw SPU problem state; run control for lv2) ---- */
enum {
    PS_SPU_STATUS_STOPPED = 0x0,
    PS_SPU_STATUS_RUNNING = 0x1,
    PS_SPU_STATUS_STOPPED_BY_STOP = 0x2,
    PS_SPU_STATUS_STOPPED_BY_HALT = 0x4,
    PS_SPU_STATUS_WAITING_FOR_CHANNEL = 0x8,
};

/* Floating-point model selector (pspocket/spu/fp.h). */
enum { PS_SPU_XFLOAT_APPROXIMATE = 0, PS_SPU_XFLOAT_ACCURATE = 1 };

/* One 128-bit register in D-004 lane order. */
typedef union __attribute__((aligned(16))) ps_spu_v128 {
    uint8_t u8[16];
    int8_t s8[16];
    uint16_t u16[8];
    int16_t s16[8];
    uint32_t u32[4];
    int32_t s32[4];
    uint64_t u64[2];
    int64_t s64[2];
    float f32[4];
    double f64[2];
} ps_spu_v128;

/* An MFC command waiting in the queue (only list commands stalled on stall-and-notify, and commands ordered behind
 * them by a fence or barrier, ever wait: every other command executes when it is issued). */
typedef struct ps_spu_mfc_cmd {
    uint32_t lsa;
    uint32_t eal;  /* list commands: LS address of the next list element */
    uint32_t eah;
    uint32_t size; /* list commands: bytes of list left */
    uint8_t tag;
    uint8_t cmd;
    uint8_t stalled; /* list waiting for MFC_WrListStallAck */
    uint8_t pad;
} ps_spu_mfc_cmd;

/* Channel-visible state. Counts are 0 or 1 except in_mbox_count (0..4). */
typedef struct ps_spu_channels {
    /* MFC command parameters (MFC_LSA, MFC_EAH, MFC_EAL, MFC_Size, MFC_TagID) */
    uint32_t mfc_lsa;
    uint32_t mfc_eah;
    uint32_t mfc_eal;
    uint32_t mfc_size;
    uint32_t mfc_tag;

    /* tag groups */
    uint32_t tag_mask;   /* MFC_WrTagMask */
    uint32_t tag_update; /* pending conditional update (ANY/ALL), 0 when none */
    uint32_t tag_stat;   /* MFC_RdTagStat */
    uint32_t tag_stat_count;
    uint32_t stall_stat; /* MFC_RdListStallStat */
    uint32_t stall_stat_count;
    uint32_t stall_mask; /* tags that have a stalled list */
    uint32_t atomic_stat; /* MFC_RdAtomicStat */
    uint32_t atomic_stat_count;

    /* MFC queue */
    uint32_t queue_size;
    uint32_t queue_fence;   /* tags with a queued command */
    uint32_t queue_barrier; /* tags a queued command blocks completely (barrier forms, barrier commands) */
    uint32_t pad0;
    ps_spu_mfc_cmd queue[16];

    /* mailboxes */
    uint32_t in_mbox[4];
    uint32_t in_mbox_count;
    uint32_t out_mbox;
    uint32_t out_mbox_count;
    uint32_t out_intr_mbox;
    uint32_t out_intr_mbox_count;

    /* signal notification registers; snr_or_mode bit n = SNR n+1 accumulates with OR instead of overwriting */
    uint32_t snr[2];
    uint32_t snr_count[2];
    uint32_t snr_or_mode;

    /* events */
    uint32_t event_mask;
    uint32_t event_stat;
    uint32_t event_count; /* SPU_RdEventStat channel count */

    /* decrementer: counts down at the timebase frequency from dec_value written at dec_start */
    uint32_t dec_value;
    uint32_t dec_frozen;
    uint64_t dec_start;

    /* lock-line reservation (GETLLAR) */
    uint32_t res_addr;
    uint32_t res_valid;
    uint64_t res_version;
    uint8_t res_data[128];
} ps_spu_channels;

struct ps_spu_host;    /* runtime-private (pspocket/spu/runtime.h) */
struct ps_spu_group;   /* thread group, for group MMIO (runtime.h) */
struct ps_spu_program; /* lifted program attached to this context (runtime.h) */

typedef struct SPUContext {
    ps_spu_v128 gpr[128];
    ps_spu_v128 fpscr;

    uint32_t pc;
    uint32_t srr0;
    uint8_t* ls;      /* 256 KB local store, 16-byte aligned */
    uint8_t* membase; /* guest memory base (host address of guest address 0), for DMA */

    /* Set by other threads (atomically) to make running code return to the run loop: stop requests and pending
     * interrupts. Lifted code polls it on loop back edges. */
    uint32_t attention;

    uint32_t stop_reason; /* PS_SPU_STOP_* */
    uint32_t stop_code;
    uint32_t exception; /* PS_SPU_EXC_* */
    uint8_t interrupts_enabled;
    uint8_t xfloat; /* PS_SPU_XFLOAT_*: floating-point model of this context (fp.h) */
    uint8_t pad1[2];

    /* identity */
    uint32_t id;    /* runtime id (lv2 assigns) */
    uint32_t index; /* index in the thread group (0..7) */

    ps_spu_channels ch;

    const struct ps_spu_program* program; /* lifted code for the current LS image, or NULL (interpreter only) */
    struct ps_spu_group* group;
    struct ps_spu_host* host; /* NULL: single-thread use (tests); blocking channel accesses stop instead */
    void* runtime;            /* owned by the lv2 layer; never touched by the SPU runtime */
} SPUContext;

/* Layout is part of the ABI between the runtime and generated code. */
PS_SPU_STATIC_ASSERT(sizeof(ps_spu_v128) == 16, "ps_spu_v128 is 128 bits");
PS_SPU_STATIC_ASSERT(offsetof(SPUContext, gpr) == 0x000, "SPUContext layout");
PS_SPU_STATIC_ASSERT(offsetof(SPUContext, fpscr) == 0x800, "SPUContext layout");
PS_SPU_STATIC_ASSERT(offsetof(SPUContext, pc) == 0x810, "SPUContext layout");
PS_SPU_STATIC_ASSERT(offsetof(SPUContext, ls) == 0x818, "SPUContext layout");
PS_SPU_STATIC_ASSERT(offsetof(SPUContext, attention) == 0x828, "SPUContext layout");
PS_SPU_STATIC_ASSERT(offsetof(SPUContext, stop_reason) == 0x82C, "SPUContext layout");
PS_SPU_STATIC_ASSERT(sizeof(ps_spu_mfc_cmd) == 20, "ps_spu_mfc_cmd layout");
PS_SPU_STATIC_ASSERT(sizeof(double) == 8, "doubles are IEEE binary64");

/* Reset state: everything zero; ls and membase as given. Floating-point model: the process default (fp.h). */
void ps_spu_context_init(SPUContext* ctx, uint8_t* ls, uint8_t* membase);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PSPOCKET_SPU_CONTEXT_H */
