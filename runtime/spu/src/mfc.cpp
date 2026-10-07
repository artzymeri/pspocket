// SPU MFC: DMA (GET/PUT and their barrier/fence/result forms, lists with stall-and-notify), sndsig, sdcrz, the
// atomic commands (GETLLAR/PUTLLC/PUTLLUC/PUTQLLUC through the shared reservation system) and thread-group MMIO.
//
// Every transfer executes synchronously when it is issued (plan section 6), so tag groups complete immediately.
// The only commands that ever wait in the queue are lists stalled on a stall-and-notify element and commands
// ordered behind them by a fence, a barrier form or a barrier command. Semantics follow RPCS3
// rpcs3/Emu/Cell/SPUThread.cpp (process_mfc_cmd, do_dma_transfer, do_list_transfer, do_putllc, do_putlluc,
// do_dma_check); the alignment rules are the CBEA's (RPCS3 does not check them):
//   size 1, 2, 4 or 8: EA aligned to the size and LS/EA low 4 bits equal; size 16*n (n <= 1024): LS and EA
//   16-byte aligned; lists: list size a multiple of 8 and <= 16 KB, list address 8-byte aligned.
//
// Called without the context lock: DMA may reach another SPU of the group (its LS or signal registers), which takes
// that context's lock. The lock is only taken here to change events.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "internal.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/core/reservation.h"
#include "pspocket/spu/runtime.h"

using namespace pspocket;
using namespace pspocket::spu::detail;

namespace {

bool is_transfer(u32 cmd)
{
    switch (cmd) {
    case PS_MFC_PUT: case PS_MFC_PUTB: case PS_MFC_PUTF:
    case PS_MFC_PUTR: case PS_MFC_PUTRB: case PS_MFC_PUTRF:
    case PS_MFC_GET: case PS_MFC_GETB: case PS_MFC_GETF:
    case PS_MFC_SDCRZ:
    case PS_MFC_SNDSIG: case PS_MFC_SNDSIGB: case PS_MFC_SNDSIGF: return true;
    default: return false;
    }
}

bool is_list(u32 cmd)
{
    switch (cmd) {
    case PS_MFC_PUTL: case PS_MFC_PUTLB: case PS_MFC_PUTLF:
    case PS_MFC_PUTRL: case PS_MFC_PUTRLB: case PS_MFC_PUTRLF:
    case PS_MFC_GETL: case PS_MFC_GETLB: case PS_MFC_GETLF: return true;
    default: return false;
    }
}

bool is_barrier_command(u32 cmd) { return cmd == PS_MFC_BARRIER || cmd == PS_MFC_EIEIO || cmd == PS_MFC_SYNC; }
bool is_get(u32 cmd) { return (cmd & 0xF0) == 0x40; }

bool dma_aligned(u32 lsa, u32 eal, u32 size)
{
    if (size == 0) return true;
    if (size < 16)
        return (size == 1 || size == 2 || size == 4 || size == 8) && (eal & (size - 1)) == 0 && (lsa & 0xF) == (eal & 0xF);
    return (size & 0xF) == 0 && (lsa & 0xF) == 0 && (eal & 0xF) == 0;
}

// Copies between the LS (wrapping at 256 KB) and a host buffer.
void ls_copy(u8* ls, u32 lsa, u8* other, u32 size, bool to_ls)
{
    u32 done = 0;
    while (done < size) {
        const u32 at = (lsa + done) & PS_SPU_LS_MASK;
        const u32 chunk = std::min(size - done, PS_SPU_LS_SIZE - at);
        // memmove: a group-MMIO transfer may target this SPU's own LS
        if (to_ls)
            std::memmove(ls + at, other + done, chunk);
        else
            std::memmove(other + done, ls + at, chunk);
        done += chunk;
    }
}

int group_mmio(SPUContext* ctx, u32 pc, u32 cmd, u32 lsa, u32 eal, u32 size)
{
    if (eal < PS_SPU_THREAD_BASE)
        return raise_exception(ctx, pc, PS_SPU_EXC_DMA_ADDRESS, eal, "DMA to raw SPU MMIO (not supported)");
    const u32 index = (eal - PS_SPU_THREAD_BASE) / PS_SPU_THREAD_OFFSET;
    const u32 offset = (eal - PS_SPU_THREAD_BASE) % PS_SPU_THREAD_OFFSET;
    SPUContext* target = (ctx->group && index < ctx->group->count) ? ctx->group->members[index] : nullptr;
    if (!target) return raise_exception(ctx, pc, PS_SPU_EXC_DMA_ADDRESS, eal, "DMA to a missing SPU thread of the group");
    const bool get = is_get(cmd);
    if (offset + size <= PS_SPU_LS_SIZE) {
        // Local store of a group member (RPCS3 do_dma_transfer, "SPU Thread Group MMIO").
        if (cmd == PS_MFC_SDCRZ) {
            std::memset(target->ls + offset, 0, size);
        } else {
            ls_copy(ctx->ls, lsa, target->ls + offset, size, get);
        }
        return 0;
    }
    if (!get && size == 4 && (offset == PS_SPU_THREAD_SNR1 || offset == PS_SPU_THREAD_SNR2)) {
        const u32 value = cmd == PS_MFC_SDCRZ ? 0 : ps_ld32(ctx->ls + (lsa & PS_SPU_LS_MASK & ~3u));
        ps_spu_write_signal(target, offset == PS_SPU_THREAD_SNR1 ? 1 : 2, value);
        return 0;
    }
    return raise_exception(ctx, pc, PS_SPU_EXC_DMA_ADDRESS, eal, "DMA to an invalid SPU thread MMIO offset");
}

// Whether a GET of size bytes at LS address at writes over program p's code where it runs (reloc past where it was
// lifted).
bool overwrites(const ps_spu_program* p, u32 reloc, u32 at, u32 size)
{
    const u32 start = (p->code_start + reloc) & PS_SPU_LS_MASK;
    const u32 into = (at - start) & PS_SPU_LS_MASK; // where the copy starts, from the program's start
    u32 first, end;                                 // the overlap, in bytes from the program's start
    if (into < p->code_size) {
        first = into;
        end = std::min<u32>(p->code_size, into + size);
    } else if (((start - at) & PS_SPU_LS_MASK) < size) {
        first = 0;
        end = std::min<u32>(p->code_size, size - ((start - at) & PS_SPU_LS_MASK));
    } else {
        return false;
    }
    if (!p->code_words) return true;
    // Only its lifted instructions matter (D-085): data and bss share the range.
    for (u32 w = first >> 2; w < (end + 3) >> 2; w++)
        if ((p->code_words[w >> 5] >> (w & 31)) & 1) return true;
    return false;
}

// A GET over lifted code (an overlay, or other code loaded where it was) leaves it stale: the rest of the run
// interprets it (D-076). Both the current program and the host's (home, D-081) are checked; SPURS attaches its program
// again when it loads it next.
void drop_overwritten_programs(SPUContext* ctx, u32 lsa, u32 size)
{
    const u32 at = lsa & PS_SPU_LS_MASK;
    const ps_spu_program* dropped = nullptr;
    if (const ps_spu_program* p = ctx->program; p && overwrites(p, ctx->reloc, at, size)) {
        PS_LOG_DEBUG("spu", "SPU {}: a DMA writes LS 0x{:05x}+{:#x} over lifted program {}: interpreting it", ctx->id, at,
            size, p->name);
        dropped = p;
        set_current(ctx, nullptr, 0);
    }
    if (const ps_spu_program* h = ctx->home; h && overwrites(h, ctx->home_reloc, at, size)) {
        if (h != dropped)
            PS_LOG_DEBUG("spu", "SPU {}: a DMA writes LS 0x{:05x}+{:#x} over lifted program {}: interpreting it", ctx->id,
                at, size, h->name);
        ctx->home = nullptr;
        ctx->home_reloc = 0;
    }
}

// SPU DMAs to and from RSX local memory, bytes per MB of it (ps_spu_video_dma): the GPU's copies of render targets
// and textures there are not told (a diagnostic for now).
std::atomic<u64> g_video_get[PS_MEM_RSX_LOCAL_SIZE >> 20], g_video_put[PS_MEM_RSX_LOCAL_SIZE >> 20];

void note_video_dma(u32 cmd, u32 eal, u32 size)
{
    const u32 mb = (eal - PS_MEM_RSX_LOCAL_BASE) >> 20;
    (is_get(cmd) ? g_video_get : g_video_put)[mb].fetch_add(size, std::memory_order_relaxed);
}

// The DMA write log (ps_spu_dma_writes_over): a ring of the last writes per host thread, registered for the search and
// dropped with its thread. A write costs a clock read and five stores. 2,048 entries, or PSPOCKET_DMA_LOG=N (finding
// an overwrite older than a few milliseconds of SPU traffic, run 100).
u32 dma_log_size()
{
    static const u32 size = [] {
        const char* e = std::getenv("PSPOCKET_DMA_LOG");
        const unsigned long n = e ? std::strtoul(e, nullptr, 0) : 0;
        return n >= 16 && n <= (1u << 24) ? static_cast<u32>(n) : 2048u;
    }();
    return size;
}

struct DmaWriteRing {
    std::vector<ps_spu_dma_write> e = std::vector<ps_spu_dma_write>(dma_log_size());
    std::atomic<u32> next{0};
    u64 tag = 0;
};
std::mutex g_rings_mutex;
std::vector<DmaWriteRing*> g_rings;
struct RingHolder {
    DmaWriteRing* ring = nullptr;
    ~RingHolder()
    {
        if (!ring) return;
        std::lock_guard lock(g_rings_mutex);
        std::erase(g_rings, ring);
        delete ring;
    }
};
thread_local RingHolder t_ring;

DmaWriteRing& dma_ring()
{
    if (!t_ring.ring) {
        t_ring.ring = new DmaWriteRing;
        std::lock_guard lock(g_rings_mutex);
        g_rings.push_back(t_ring.ring);
    }
    return *t_ring.ring;
}

void log_write(u32 cmd, u32 pc, u32 ea, u32 size)
{
    DmaWriteRing& r = dma_ring();
    const u32 i = r.next.load(std::memory_order_relaxed);
    const u64 now = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    r.e[i % r.e.size()] = ps_spu_dma_write{ea, size, r.tag, now, pc, cmd};
    r.next.store(i + 1, std::memory_order_release);
}

// PSPOCKET_SPU_WATCH=LO-HI: every SPU write into guest [LO, HI) is logged with its command and SPU position (finding
// what corrupts a PPU stack or a structure; dev only).
struct SpuWatch {
    u32 lo = 0, hi = 0;
    SpuWatch()
    {
        if (const char* e = std::getenv("PSPOCKET_SPU_WATCH")) {
            char* end = nullptr;
            lo = static_cast<u32>(std::strtoul(e, &end, 0));
            if (end && *end == '-') hi = static_cast<u32>(std::strtoul(end + 1, nullptr, 0));
        }
    }
};
const SpuWatch& spu_watch()
{
    static const SpuWatch w;
    return w;
}

int transfer(SPUContext* ctx, u32 pc, u32 cmd, u32 lsa, u32 eal, u32 size)
{
    if (const SpuWatch& w = spu_watch(); w.hi && !is_get(cmd) && eal < w.hi && eal + size > w.lo) {
        PS_LOG_WARN("spu", "SPU write into the watched range: cmd {:#x} at SPU pc {:#07x}, LSA {:#07x} -> EA {:#010x}, size {:#x}",
            cmd, pc, lsa, eal, size);
    }
    if (size == 0) return 0;
    if (eal >= PS_SPU_RAW_BASE) return group_mmio(ctx, pc, cmd, lsa, eal, size);
    if (eal - PS_MEM_RSX_LOCAL_BASE < PS_MEM_RSX_LOCAL_SIZE) note_video_dma(cmd, eal, size);
    u8* mem = ctx->membase + eal;
    // The MFC performs a queue's transfers in order, and a fenced PUT or a later command's flag must not be seen by
    // another processor before the data PUT before it: on ARM, plain stores can become visible out of order, so
    // every PUT is a release (D-077: GTA V's graphics jobs write RSX commands, then the jump that releases them).
    if (!is_get(cmd)) {
        std::atomic_thread_fence(std::memory_order_release);
        ctx->mem_writes++;
        log_write(cmd, pc, eal, size);
    }
    if (cmd == PS_MFC_SDCRZ) {
        std::memset(mem, 0, size);
    } else if (!is_get(cmd) && size > 4) {
        // The first doubleword last, as one release store (D-124, D-225). GTA V's graphics jobs write each block of RSX
        // commands with one PUT, from 16 bytes to 4.5 KB, over the jump-to-self the RSX is parked on: the block's first
        // word. A plain copy's stores can reach another core in any order, and the RSX, seeing that word change, read on
        // into words not written yet (the iPhone's FIFO desyncs, several a minute in the first mission's escape).
        // The first 8 bytes go in one store, not as two words: an aligned 8-byte DMA lands whole on the Cell, and GTA V's
        // jobs write job chain commands with 8-byte PUTs. Written word by word, a jump-to-self (0x00000008_00000012) over
        // an END showed its low word alone for a moment, 0x12, a light sync; the job chain passed it and ran the previous
        // lap's commands, whose descriptors the game had reused (the EDGE/GFX stops since D-124: D-130, D-138, runs 095,
        // 100, repro-edge, runs 173-179).
        if (size > 8) ls_copy(ctx->ls, lsa + 8, mem + 8, size - 8, false);
        u64 head; // its bytes as they are
        std::memcpy(&head, ctx->ls + (lsa & PS_SPU_LS_MASK), 8);
        __atomic_store_n(reinterpret_cast<u64*>(mem), head, __ATOMIC_RELEASE);
    } else {
        ls_copy(ctx->ls, lsa, mem, size, is_get(cmd));
        if (is_get(cmd)) {
            // The GET's loads are done before any later one (D-130): an SPU that GETs a flag, waits for it and then GETs
            // the data the flag announces must not have read the data first (ARM reorders loads; the MFC's GET
            // completed before the SPU looked).
            std::atomic_thread_fence(std::memory_order_acquire);
            if (ctx->program || ctx->home) drop_overwritten_programs(ctx, lsa, size);
            observe_get(ctx, lsa, eal, size); // code a program loads itself (D-079)
        }
    }
    if (!is_get(cmd)) ps_res_notify_write(eal, size); // PUTs break reservations on the lines they touch
    return 0;
}

void mark_stalled(SPUContext* ctx, u32 tag)
{
    ps_spu_channels& ch = ctx->ch;
    const u32 bit = 1u << tag;
    ch.stall_mask |= bit;
    ChannelLock lock(ctx);
    if (!ch.stall_stat_count) set_events(ctx, PS_SPU_EVENT_SN);
    ch.stall_stat |= bit;
    ch.stall_stat_count = 1;
}

// Runs list elements of c until the list ends (0), a stall-and-notify element stalls it (2) or an exception (1).
int run_list(SPUContext* ctx, u32 pc, ps_spu_mfc_cmd& c)
{
    const u32 base_cmd = c.cmd & ~u32{PS_MFC_LIST_MASK | PS_MFC_BARRIER_MASK | PS_MFC_FENCE_MASK};
    while (c.size >= 8) {
        const u32 at = c.eal & 0x3FFF8u;
        const u32 w0 = ps_ld32(ctx->ls + at);
        const u32 ea = ps_ld32(ctx->ls + at + 4);
        const bool stall = (w0 >> 31) & 1;
        const u32 size = w0 & 0x7FFFu;
        const u32 lsa = (c.lsa & 0x3FFF0u) | (ea & 0xFu);
        if (size > PS_MFC_MAX_SIZE)
            return raise_exception(ctx, pc, PS_SPU_EXC_DMA_COMMAND, c.cmd, "DMA list element larger than 16 KB");
        if (!dma_aligned(lsa, ea, size))
            return raise_exception(ctx, pc, PS_SPU_EXC_DMA_ALIGNMENT, c.cmd, "misaligned DMA list element");
        if (transfer(ctx, pc, base_cmd, lsa, ea, size)) return 1;
        c.lsa = ((c.lsa & 0x3FFF0u) + ((size + 15) & ~15u)) & PS_SPU_LS_MASK;
        c.eal = (c.eal + 8) & PS_SPU_LS_MASK;
        c.size -= 8;
        // RPCS3 do_list_transfer: the element with the stall bit is transferred, then the list stalls (unless it
        // was the last element).
        if (stall && c.size >= 8) {
            c.stalled = 1;
            mark_stalled(ctx, c.tag);
            return 2;
        }
    }
    c.size = 0;
    return 0;
}

bool blocked(const ps_spu_mfc_cmd& c, u32 fence, u32 barrier)
{
    const u32 bit = 1u << c.tag;
    if (barrier & bit) return true;
    return (c.cmd & (PS_MFC_BARRIER_MASK | PS_MFC_FENCE_MASK)) && (fence & bit);
}

void add_masks(const ps_spu_mfc_cmd& c, u32& fence, u32& barrier)
{
    const u32 bit = 1u << c.tag;
    fence |= bit;
    if (is_barrier_command(c.cmd))
        barrier = ~0u;
    else if (c.cmd & PS_MFC_BARRIER_MASK)
        barrier |= bit;
}

int enqueue(SPUContext* ctx, u32 pc, const ps_spu_mfc_cmd& c)
{
    ps_spu_channels& ch = ctx->ch;
    if (ch.queue_size >= 16)
        return raise_exception(ctx, pc, PS_SPU_EXC_DEADLOCK, PS_MFC_Cmd, "MFC queue full of stalled commands");
    ch.queue[ch.queue_size++] = c;
    add_masks(c, ch.queue_fence, ch.queue_barrier);
    return 0;
}

int putlluc(SPUContext* ctx, u32 pc, u32 eal, u32 lsa)
{
    const u32 addr = eal & ~127u;
    if (addr >= PS_SPU_RAW_BASE) return raise_exception(ctx, pc, PS_SPU_EXC_DMA_ADDRESS, eal, "PUTLLUC outside main memory");
    ps_spu_channels& ch = ctx->ch;
    if (ch.res_valid && ch.res_addr == addr) {
        // RPCS3 do_putlluc: storing over our own reservation consumes it; LR only if it was already lost.
        ChannelLock lock(ctx);
        if (reservation_lost(ctx)) set_events(ctx, PS_SPU_EVENT_LR);
        ch.res_valid = 0;
    }
    ps_res_store_line_unconditional(ctx->membase, addr, ctx->ls + (lsa & 0x3FF80u));
    ctx->mem_writes++;
    log_write(PS_MFC_PUTLLUC, pc, addr, 128);
    return 0;
}

int execute_queued(SPUContext* ctx, u32 pc, ps_spu_mfc_cmd& c)
{
    if (is_list(c.cmd)) return run_list(ctx, pc, c);
    if (c.cmd == PS_MFC_PUTQLLUC) return putlluc(ctx, pc, c.eal, c.lsa);
    if (is_barrier_command(c.cmd)) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return 0;
    }
    return transfer(ctx, pc, c.cmd, c.lsa, c.eal, c.size);
}

// Runs every queued command that is no longer blocked, in order.
int process_queue(SPUContext* ctx, u32 pc)
{
    ps_spu_channels& ch = ctx->ch;
    ps_spu_mfc_cmd kept[16];
    u32 count = 0, fence = 0, barrier = 0;
    for (u32 i = 0; i < ch.queue_size; i++) {
        ps_spu_mfc_cmd c = ch.queue[i];
        const bool waits = c.stalled || (is_barrier_command(c.cmd) ? count != 0 : blocked(c, fence, barrier));
        if (!waits) {
            const int r = execute_queued(ctx, pc, c);
            if (r == 1) return 1;
            if (r == 0) continue; // done
        }
        kept[count++] = c;
        add_masks(c, fence, barrier);
    }
    ch.queue_size = count;
    std::copy(kept, kept + count, ch.queue);
    ch.queue_fence = fence;
    ch.queue_barrier = barrier;
    return 0;
}

} // namespace

namespace pspocket::spu::detail {

void mfc_update_tag_status(SPUContext* ctx)
{
    ps_spu_channels& ch = ctx->ch;
    if (!ch.tag_update) return;
    const u32 completed = mfc_completed(ctx);
    if ((ch.tag_update == PS_MFC_TAG_UPDATE_ANY && completed) ||
        (ch.tag_update == PS_MFC_TAG_UPDATE_ALL && completed == ch.tag_mask)) {
        ch.tag_stat = completed;
        ch.tag_stat_count = 1;
        ch.tag_update = 0;
    }
}

int mfc_stall_ack(SPUContext* ctx, u32 pc, u32 tag)
{
    ps_spu_channels& ch = ctx->ch;
    const u32 bit = 1u << tag;
    if (!(ch.stall_mask & bit)) return 0;
    ch.stall_mask &= ~bit;
    for (u32 i = 0; i < ch.queue_size; i++)
        if (ch.queue[i].tag == tag) ch.queue[i].stalled = 0;
    const int r = process_queue(ctx, pc);
    mfc_update_tag_status(ctx);
    return r;
}

int mfc_command(SPUContext* ctx, u32 pc, u32 cmd)
{
    ps_spu_channels& ch = ctx->ch;
    ps_spu_mfc_cmd c{};
    c.lsa = ch.mfc_lsa;
    c.eal = ch.mfc_eal;
    c.eah = ch.mfc_eah;
    c.size = ch.mfc_size;
    c.tag = static_cast<u8>(ch.mfc_tag & 0x1F);
    c.cmd = static_cast<u8>(cmd);

    int result = 0;
    switch (cmd) {
    case PS_MFC_SDCRT:
    case PS_MFC_SDCRTST:
    case PS_MFC_SDCRS:
    case PS_MFC_SDCRF: return 0; // cache hints: no caches to manage

    case PS_MFC_GETLLAR: {
        const u32 addr = c.eal & ~127u;
        if (addr >= PS_SPU_RAW_BASE) return raise_exception(ctx, pc, PS_SPU_EXC_DMA_ADDRESS, c.eal, "GETLLAR outside main memory");
        if (ch.res_valid) {
            // Replacing a reservation that was lost raises LR first (RPCS3 process_mfc_cmd, GETLLAR).
            ChannelLock lock(ctx);
            if (reservation_lost(ctx)) set_events(ctx, PS_SPU_EVENT_LR);
        }
        alignas(16) u8 line[128];
        const u64 version = ps_res_acquire(ctx->membase, addr, line);
        std::memcpy(ctx->ls + (c.lsa & 0x3FF80u), line, 128);
        std::memcpy(ch.res_data, line, 128);
        ch.res_addr = addr;
        ch.res_version = version;
        ch.res_valid = 1;
        ch.atomic_stat = PS_MFC_GETLLAR_SUCCESS;
        ch.atomic_stat_count = 1;
        return 0;
    }
    case PS_MFC_PUTLLC: {
        const u32 addr = c.eal & ~127u;
        if (addr >= PS_SPU_RAW_BASE) return raise_exception(ctx, pc, PS_SPU_EXC_DMA_ADDRESS, c.eal, "PUTLLC outside main memory");
        const bool ok = ch.res_valid && ch.res_addr == addr &&
            ps_res_store_line(ctx->membase, addr, ch.res_version, ch.res_data, ctx->ls + (c.lsa & 0x3FF80u));
        if (!ok && ch.res_valid) {
            // RPCS3 do_putllc: a failed PUTLLC on the reserved line means it was lost; another line is checked.
            ChannelLock lock(ctx);
            if (ch.res_addr == addr || reservation_lost(ctx)) set_events(ctx, PS_SPU_EVENT_LR);
        }
        ch.res_valid = 0;
        if (ok) {
            ctx->mem_writes++;
            log_write(PS_MFC_PUTLLC, pc, addr, 128);
        }
        ch.atomic_stat = ok ? PS_MFC_PUTLLC_SUCCESS : PS_MFC_PUTLLC_FAILURE;
        ch.atomic_stat_count = 1;
        return 0;
    }
    case PS_MFC_PUTLLUC:
        if (putlluc(ctx, pc, c.eal, c.lsa)) return 1;
        ch.atomic_stat = PS_MFC_PUTLLUC_SUCCESS;
        ch.atomic_stat_count = 1;
        return 0;
    case PS_MFC_PUTQLLUC:
        result = blocked(c, ch.queue_fence, ch.queue_barrier) ? enqueue(ctx, pc, c) : putlluc(ctx, pc, c.eal, c.lsa);
        break;
    case PS_MFC_BARRIER:
    case PS_MFC_EIEIO:
    case PS_MFC_SYNC:
        if (ch.queue_size == 0)
            std::atomic_thread_fence(std::memory_order_seq_cst);
        else
            result = enqueue(ctx, pc, c);
        break;
    default:
        if (is_transfer(cmd)) {
            if ((cmd & 0xF0) == 0xA0 && c.size != 4)
                return raise_exception(ctx, pc, PS_SPU_EXC_DMA_COMMAND, cmd, "sndsig with a size other than 4");
            if (c.size > PS_MFC_MAX_SIZE)
                return raise_exception(ctx, pc, PS_SPU_EXC_DMA_COMMAND, cmd, "DMA larger than 16 KB");
            if (!dma_aligned(c.lsa & PS_SPU_LS_MASK, c.eal, c.size))
                return raise_exception(ctx, pc, PS_SPU_EXC_DMA_ALIGNMENT, cmd, "misaligned DMA");
            if (blocked(c, ch.queue_fence, ch.queue_barrier))
                result = enqueue(ctx, pc, c);
            else
                result = transfer(ctx, pc, cmd, c.lsa, c.eal, c.size);
        } else if (is_list(cmd)) {
            if (c.size > PS_MFC_MAX_SIZE)
                return raise_exception(ctx, pc, PS_SPU_EXC_DMA_COMMAND, cmd, "DMA list larger than 16 KB");
            if ((c.size & 7) || (c.eal & 7))
                return raise_exception(ctx, pc, PS_SPU_EXC_DMA_ALIGNMENT, cmd, "misaligned DMA list");
            c.eal &= PS_SPU_LS_MASK;
            if (blocked(c, ch.queue_fence, ch.queue_barrier)) {
                result = enqueue(ctx, pc, c);
            } else {
                const int r = run_list(ctx, pc, c);
                if (r == 1) return 1;
                if (r == 2) result = enqueue(ctx, pc, c);
            }
        } else {
            return raise_exception(ctx, pc, PS_SPU_EXC_DMA_COMMAND, cmd, "invalid MFC command");
        }
        break;
    }
    if (result) return result;
    mfc_update_tag_status(ctx);
    return 0;
}

} // namespace pspocket::spu::detail

extern "C" uint64_t ps_spu_video_dma(uint32_t mb, int put)
{
    if (mb >= (PS_MEM_RSX_LOCAL_SIZE >> 20)) return 0;
    return (put ? g_video_put : g_video_get)[mb].load(std::memory_order_relaxed);
}

extern "C" void ps_spu_set_dma_tag(uint64_t tag) { dma_ring().tag = tag; }

extern "C" uint32_t ps_spu_dma_writes_over(uint32_t ea, uint32_t size, ps_spu_dma_write* out, uint32_t n)
{
    // Read while the rings' threads write on: an entry being written may show torn, which a diagnostic accepts.
    std::vector<ps_spu_dma_write> found;
    {
        std::lock_guard lock(g_rings_mutex);
        for (const DmaWriteRing* r : g_rings) {
            const u32 next = r->next.load(std::memory_order_acquire);
            const u32 ring = static_cast<u32>(r->e.size());
            const u32 count = std::min<u32>(next, ring);
            for (u32 k = 0; k < count; k++) {
                const ps_spu_dma_write& w = r->e[(next - 1 - k) % ring];
                if (u64{w.ea} < u64{ea} + size && ea < u64{w.ea} + w.size) found.push_back(w);
            }
        }
    }
    std::sort(found.begin(), found.end(), [](const ps_spu_dma_write& a, const ps_spu_dma_write& b) {
        return a.time_ns > b.time_ns;
    });
    const u32 count = std::min<u32>(n, static_cast<u32>(found.size()));
    std::copy_n(found.begin(), count, out);
    return count;
}
