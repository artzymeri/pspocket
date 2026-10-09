// SPU channels: mailboxes, signal notification, events, decrementer, SRR0, the MFC parameter and status channels,
// stop-and-signal, halt, interrupts, and the PPU-side accessors. Semantics follow RPCS3 rpcs3/Emu/Cell/SPUThread.cpp
// (get_ch_value, set_ch_value, get_ch_count, get_events, set_events, stop_and_signal), simplified for a runtime
// where every DMA completes when it is issued.
//
// Blocking: a channel access that cannot complete waits on the context's condition variable (the SPU's host thread
// sleeps). Reads that only the SPU itself could satisfy (tag status, list stall status, atomic status with nothing
// pending) can never complete here, so they raise PS_SPU_EXC_DEADLOCK instead of hanging (CLAUDE.md rule 5).

#include <algorithm>
#include <chrono>
#include <cstring>

#include "internal.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/core/reservation.h"
#include "pspocket/core/timebase.h"
#include "pspocket/spu/fp.h"
#include "pspocket/spu/runtime.h"
#include "pspocket/spu/thread.hpp"

using namespace pspocket;
using namespace pspocket::spu::detail;

namespace {

std::atomic<ps_spu_stop_handler> g_stop_handler{nullptr};
std::atomic<ps_spu_intr_mbox_handler> g_intr_handler{nullptr};
std::atomic<ps_spu_timebase_source> g_timebase{nullptr};

// How long an event wait sleeps in ps_res_wait_change before it re-checks the other events and the line's bytes
// (plain guest stores don't bump reservation versions). Bounds the latency of mailbox and signal events while an
// LR wait is active.
constexpr u64 kReservationPollNs = 100'000;

int block_stop(SPUContext* ctx, u32 pc)
{
    ctx->pc = pc;
    ctx->stop_reason = PS_SPU_STOP_BLOCKED;
    return 1;
}

int requested_stop(SPUContext* ctx, u32 pc)
{
    ctx->pc = pc;
    ctx->stop_reason = PS_SPU_STOP_REQUESTED;
    return 1;
}

bool stop_requested(const SPUContext* ctx)
{
    return ctx->host && ctx->host->stop_requested.load(std::memory_order_acquire);
}

// Waits until ready() holds; 0 with the lock held, or 1 when the SPU must stop first.
template <class Ready>
int wait_until(SPUContext* ctx, ChannelLock& lock, u32 pc, Ready ready)
{
    if (ready()) return 0;
    if (!ctx->host) return block_stop(ctx, pc);
    ps_spu_host* host = ctx->host;
    host->status.fetch_or(PS_SPU_STATUS_WAITING_FOR_CHANNEL, std::memory_order_relaxed);
    int result = 0;
    while (!ready()) {
        if (stop_requested(ctx)) {
            result = requested_stop(ctx, pc);
            break;
        }
        host->cv.wait(lock.lock());
    }
    host->status.fetch_and(~u32{PS_SPU_STATUS_WAITING_FOR_CHANNEL}, std::memory_order_relaxed);
    return result;
}

u32 dec_now(const SPUContext* ctx)
{
    if (ctx->ch.dec_frozen) return ctx->ch.dec_value;
    return static_cast<u32>(ctx->ch.dec_value - (ps_spu_timebase() - ctx->ch.dec_start));
}

// Nanoseconds until the decrementer underflows (0 when frozen or already pending).
u64 ns_until_underflow(const SPUContext* ctx)
{
    if (ctx->ch.dec_frozen) return 0;
    const u64 elapsed = ps_spu_timebase() - ctx->ch.dec_start;
    const u64 target = u64{ctx->ch.dec_value} + 1;
    if (elapsed >= target) return 1000;
    const unsigned __int128 ticks = target - elapsed;
    return static_cast<u64>(ticks * 1'000'000'000ull / PS_TIMEBASE_HZ) + 1000;
}

void take_interrupt(SPUContext* ctx)
{
    // RPCS3 check_mfc_interrupts: SRR0 = the interrupted pc, handler at LS 0 (a br/bra there is followed).
    ctx->srr0 = ctx->pc;
    ctx->interrupts_enabled = 0;
    u32 br = 0;
    std::memcpy(&br, ctx->ls, 4);
    br = __builtin_bswap32(br);
    ctx->pc = (br & 0xFD80007Fu) == 0x30000000u ? (br >> 5) & 0x3FFFCu : 0;
}

void push_signal(SPUContext* ctx, u32 index, u32 value)
{
    ps_spu_channels& ch = ctx->ch;
    if ((ch.snr_or_mode >> index) & 1)
        ch.snr[index] |= value;
    else
        ch.snr[index] = value;
    ch.snr_count[index] = 1;
    set_events(ctx, index == 0 ? PS_SPU_EVENT_S1 : PS_SPU_EVENT_S2);
}

int read_event_stat(SPUContext* ctx, ChannelLock& lock, u32 pc, u32* value)
{
    for (;;) {
        const u32 mask = ctx->ch.event_mask;
        collect_events(ctx, mask);
        if (ctx->ch.event_count) {
            ctx->ch.event_count = 0;
            *value = ctx->ch.event_stat & mask;
            return 0;
        }
        if (!ctx->host) return block_stop(ctx, pc);
        if (stop_requested(ctx)) return requested_stop(ctx, pc);

        ps_spu_host* host = ctx->host;
        host->status.fetch_or(PS_SPU_STATUS_WAITING_FOR_CHANNEL, std::memory_order_relaxed);
        const u64 dec_ns = (mask & PS_SPU_EVENT_TM) ? ns_until_underflow(ctx) : 0;
        if ((mask & PS_SPU_EVENT_LR) && ctx->ch.res_valid) {
            // Reservation-lost wait: sleep on the line's version (wakes on conditional stores, DMA PUTs and
            // ps_res_notify_write), polling the rest at kReservationPollNs.
            const u32 addr = ctx->ch.res_addr;
            const u64 version = ctx->ch.res_version;
            lock.unlock();
            ps_res_wait_change(addr, version, dec_ns ? std::min(dec_ns, kReservationPollNs) : kReservationPollNs);
            lock.relock();
        } else if (dec_ns) {
            host->cv.wait_for(lock.lock(), std::chrono::nanoseconds(dec_ns));
        } else {
            host->cv.wait(lock.lock());
        }
        host->status.fetch_and(~u32{PS_SPU_STATUS_WAITING_FOR_CHANNEL}, std::memory_order_relaxed);
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// detail
// ---------------------------------------------------------------------------------------------------------------

namespace pspocket::spu::detail {

int raise_exception(SPUContext* ctx, u32 pc, u32 exception, u32 detail, const char* what)
{
    ctx->pc = pc & PS_SPU_PC_MASK;
    ctx->stop_reason = PS_SPU_STOP_EXCEPTION;
    ctx->exception = exception;
    ctx->stop_code = detail;
    PS_LOG_ERROR("spu", "SPU {} exception at 0x{:05x}: {} (detail 0x{:x})", ctx->id, pc, what, detail);
    return 1;
}

void set_events(SPUContext* ctx, u32 bits)
{
    ps_spu_channels& ch = ctx->ch;
    ch.event_stat |= bits;
    if (ch.event_mask & bits) {
        // RPCS3 set_events: a masked event sets the channel count even if its bit was already set.
        ch.event_count = 1;
        if (ctx->interrupts_enabled) __atomic_fetch_or(&ctx->attention, PS_SPU_ATTN_INTERRUPT, __ATOMIC_RELEASE);
        notify(ctx);
    }
}

bool reservation_lost(const SPUContext* ctx)
{
    const ps_spu_channels& ch = ctx->ch;
    if (ps_res_version(ch.res_addr) != ch.res_version) return true;
    const u64* line = reinterpret_cast<const u64*>(ctx->membase + ch.res_addr);
    u64 snapshot[16];
    std::memcpy(snapshot, ch.res_data, 128);
    for (u32 i = 0; i < 16; i++)
        if (__atomic_load_n(line + i, __ATOMIC_RELAXED) != snapshot[i]) return true;
    return false;
}

void collect_events(SPUContext* ctx, u32 mask_hint)
{
    ps_spu_channels& ch = ctx->ch;
    u32 collect = 0;
    if ((mask_hint & PS_SPU_EVENT_LR) && ch.res_valid && reservation_lost(ctx)) {
        collect |= PS_SPU_EVENT_LR;
        ch.res_valid = 0;
    }
    if ((mask_hint & PS_SPU_EVENT_TM) && !ch.dec_frozen) {
        // RPCS3 get_events: the event fires when the count passes below zero (upper half of the 64-bit difference);
        // the start is moved by 2^32 ticks per wrap so the next event comes one full period later.
        const u64 res = u64{ch.dec_value} - (ps_spu_timebase() - ch.dec_start);
        if (const u64 hi = res >> 32) {
            ch.dec_start -= hi << 32;
            collect |= PS_SPU_EVENT_TM;
        }
    }
    if (collect) set_events(ctx, collect);
}

void check_interrupt(SPUContext* ctx)
{
    ChannelLock lock(ctx);
    if (!ctx->interrupts_enabled) return;
    collect_events(ctx, ctx->ch.event_mask);
    if (ctx->ch.event_count) take_interrupt(ctx);
}

void set_status(SPUContext* ctx, u32 status)
{
    if (ctx->host) ctx->host->status.store(status, std::memory_order_release);
}

HostFpScope::HostFpScope()
{
#if defined(__aarch64__)
    // Leave the SPU environment for hook code: lv2 runs with the host's default FPCR.
    const u64 current = ps_spu_fpcr_read();
    const u64 host = current & ~(PS_SPU_FPCR_RMODE | PS_SPU_FPCR_FZ | PS_SPU_FPCR_DN);
    if (host != current) {
        saved_ = current;
        restore_ = true;
        ps_spu_fpcr_write(host);
    }
#endif
}

HostFpScope::~HostFpScope()
{
#if defined(__aarch64__)
    if (restore_) ps_spu_fpcr_write(saved_);
#endif
}

} // namespace pspocket::spu::detail

// ---------------------------------------------------------------------------------------------------------------
// C API
// ---------------------------------------------------------------------------------------------------------------

extern "C" {

void ps_spu_context_init(SPUContext* ctx, uint8_t* ls, uint8_t* membase)
{
    std::memset(static_cast<void*>(ctx), 0, sizeof(SPUContext));
    ctx->ls = ls;
    ctx->membase = membase;
    ctx->xfloat = ps_spu_default_xfloat();
    ctx->ch.dec_start = ps_spu_timebase();
}

void ps_spu_thread_reset(SPUContext* ctx, uint32_t dec_value)
{
    pspocket::spu::detail::ChannelLock lock(ctx);
    ps_spu_channels& ch = ctx->ch;
    uint32_t in_mbox[4];
    std::memcpy(in_mbox, ch.in_mbox, sizeof in_mbox);
    const uint32_t in_mbox_count = ch.in_mbox_count;
    const uint32_t snr[2] = {ch.snr[0], ch.snr[1]}, snr_count[2] = {ch.snr_count[0], ch.snr_count[1]};
    const uint32_t snr_or_mode = ch.snr_or_mode;
    std::memset(static_cast<void*>(&ch), 0, sizeof ch);
    std::memcpy(ch.in_mbox, in_mbox, sizeof in_mbox);
    ch.in_mbox_count = in_mbox_count;
    ch.snr[0] = snr[0];
    ch.snr[1] = snr[1];
    ch.snr_count[0] = snr_count[0];
    ch.snr_count[1] = snr_count[1];
    ch.snr_or_mode = snr_or_mode;
    ch.dec_start = ps_spu_timebase();
    ch.dec_value = dec_value;
    std::memset(static_cast<void*>(ctx->gpr), 0, sizeof ctx->gpr);
    ctx->gpr[1].u32[3] = 0x3FFF0;
    ctx->fpscr = ps_spu_v128{};
    ctx->srr0 = 0;
    ctx->pc = 0;
    ctx->stop_reason = PS_SPU_STOP_NONE;
    ctx->stop_code = 0;
    ctx->exception = 0;
    ctx->interrupts_enabled = 0;
    ctx->program = nullptr;
    ctx->home = nullptr;
    ctx->reloc = 0;
    ctx->home_reloc = 0;
    pspocket::spu::detail::disarm_pending(ctx);
}

void ps_spu_clear_in_mbox(SPUContext* ctx)
{
    pspocket::spu::detail::ChannelLock lock(ctx);
    ctx->ch.in_mbox_count = 0;
}

void ps_spu_set_stop_handler(ps_spu_stop_handler handler) { g_stop_handler.store(handler); }
void ps_spu_set_intr_mbox_handler(ps_spu_intr_mbox_handler handler) { g_intr_handler.store(handler); }
void ps_spu_set_timebase_source(ps_spu_timebase_source source) { g_timebase.store(source); }

uint64_t ps_spu_timebase(void)
{
    const ps_spu_timebase_source source = g_timebase.load(std::memory_order_relaxed);
    return source ? source() : ps_timebase_now();
}

void ps_spu_host_attach(SPUContext* ctx)
{
    if (!ctx->host) ctx->host = new ps_spu_host();
}

void ps_spu_host_detach(SPUContext* ctx)
{
    delete ctx->host;
    ctx->host = nullptr;
}

// ---- runtime calls ----

int ps_spu_rdch(SPUContext* ctx, uint32_t pc, uint32_t channel, uint32_t* value)
{
    ps_spu_channels& ch = ctx->ch;
    // State only this SPU's own thread writes (its channels, the run loop, the SPURS kernel running on it): read
    // without the lock (D-128). GTA V's jobs read the decrementer millions of times a second.
    switch (channel) {
    case PS_SPU_RdSRR0: *value = ctx->srr0; return 0;
    case PS_SPU_RdEventMask: *value = ch.event_mask; return 0;
    case PS_MFC_RdTagMask: *value = ch.tag_mask; return 0;
    case PS_SPU_RdMachStat: *value = ctx->interrupts_enabled; return 0; // bit 0: interrupts enabled
    case PS_SPU_RdDec: *value = dec_now(ctx); return 0;
    default: break;
    }
    ChannelLock lock(ctx);
    switch (channel) {
    case PS_MFC_RdTagStat:
        if (!ch.tag_stat_count)
            return raise_exception(ctx, pc, PS_SPU_EXC_DEADLOCK, channel, "MFC_RdTagStat with no completion pending");
        ch.tag_stat_count = 0;
        *value = ch.tag_stat;
        return 0;
    case PS_MFC_RdAtomicStat:
        if (!ch.atomic_stat_count)
            return raise_exception(ctx, pc, PS_SPU_EXC_DEADLOCK, channel, "MFC_RdAtomicStat with no atomic command done");
        ch.atomic_stat_count = 0;
        *value = ch.atomic_stat;
        return 0;
    case PS_MFC_RdListStallStat:
        if (!ch.stall_stat_count)
            return raise_exception(ctx, pc, PS_SPU_EXC_DEADLOCK, channel, "MFC_RdListStallStat with no stalled list");
        ch.stall_stat_count = 0;
        *value = ch.stall_stat;
        ch.stall_stat = 0;
        return 0;
    case PS_SPU_RdInMbox: {
        if (wait_until(ctx, lock, pc, [&] { return ch.in_mbox_count != 0; })) return 1;
        *value = ch.in_mbox[0];
        ch.in_mbox[0] = ch.in_mbox[1];
        ch.in_mbox[1] = ch.in_mbox[2];
        ch.in_mbox[2] = ch.in_mbox[3];
        ch.in_mbox[3] = 0;
        ch.in_mbox_count--;
        notify(ctx);
        return 0;
    }
    case PS_SPU_RdSigNotify1:
    case PS_SPU_RdSigNotify2: {
        const u32 i = channel == PS_SPU_RdSigNotify1 ? 0 : 1;
        if (wait_until(ctx, lock, pc, [&] { return ch.snr_count[i] != 0; })) return 1;
        *value = ch.snr[i];
        ch.snr[i] = 0; // reading clears the register
        ch.snr_count[i] = 0;
        return 0;
    }
    case PS_SPU_RdEventStat: return read_event_stat(ctx, lock, pc, value);
    default: return raise_exception(ctx, pc, PS_SPU_EXC_CHANNEL, channel, "rdch of an invalid channel");
    }
}

int ps_spu_rchcnt(SPUContext* ctx, uint32_t pc, uint32_t channel, uint32_t* count)
{
    ChannelLock lock(ctx);
    const ps_spu_channels& ch = ctx->ch;
    switch (channel) {
    case PS_SPU_WrOutMbox: *count = ch.out_mbox_count ^ 1; return 0;
    case PS_SPU_WrOutIntrMbox: *count = ch.out_intr_mbox_count ^ 1; return 0;
    case PS_SPU_RdInMbox: *count = ch.in_mbox_count; return 0;
    case PS_MFC_RdTagStat: *count = ch.tag_stat_count; return 0;
    case PS_MFC_RdListStallStat: *count = ch.stall_stat_count; return 0;
    case PS_SPU_RdSigNotify1: *count = ch.snr_count[0]; return 0;
    case PS_SPU_RdSigNotify2: *count = ch.snr_count[1]; return 0;
    case PS_MFC_RdAtomicStat: *count = ch.atomic_stat_count; return 0;
    case PS_SPU_RdEventStat:
        collect_events(ctx, PS_SPU_EVENT_ALL);
        *count = ctx->ch.event_count;
        return 0;
    case PS_MFC_Cmd: *count = 16 - ch.queue_size; return 0;
    case PS_MFC_WrTagUpdate:
    case PS_SPU_WrEventMask:
    case PS_SPU_WrEventAck:
    case PS_SPU_WrDec:
    case PS_SPU_RdDec:
    case PS_MFC_WrMSSyncReq:
    case PS_SPU_RdEventMask:
    case PS_SPU_RdMachStat:
    case PS_SPU_WrSRR0:
    case PS_SPU_RdSRR0:
    case PS_SPU_Set_Bkmk_Tag:
    case PS_SPU_PM_Start_Ev:
    case PS_SPU_PM_Stop_Ev:
    case PS_MFC_RdTagMask:
    case PS_MFC_LSA:
    case PS_MFC_EAH:
    case PS_MFC_EAL:
    case PS_MFC_Size:
    case PS_MFC_TagID:
    case PS_MFC_WrTagMask:
    case PS_MFC_WrListStallAck: *count = 1; return 0;
    default: return raise_exception(ctx, pc, PS_SPU_EXC_CHANNEL, channel, "rchcnt of an invalid channel");
    }
}

int ps_spu_wrch(SPUContext* ctx, uint32_t pc, uint32_t channel, uint32_t value)
{
    ps_spu_channels& ch = ctx->ch;
    // A DMA's parameters are state only this SPU's thread reads (mfc_command): no lock for them (D-128), five per DMA.
    switch (channel) {
    case PS_SPU_WrSRR0: ctx->srr0 = value & PS_SPU_PC_MASK; return 0;
    case PS_MFC_LSA: ch.mfc_lsa = value; return 0;
    case PS_MFC_EAH: ch.mfc_eah = value; return 0;
    case PS_MFC_EAL: ch.mfc_eal = value; return 0;
    case PS_MFC_Size: ch.mfc_size = std::min<u32>(value, 0xFFFF); return 0;
    case PS_MFC_TagID: ch.mfc_tag = value & 0x1F; return 0;
    case PS_MFC_Cmd: return mfc_command(ctx, pc, value & 0xFF); // DMA may take another context's lock (group MMIO);
                                                                // mfc.cpp locks for events itself
    default: break;
    }
    ChannelLock lock(ctx);
    switch (channel) {
    case PS_MFC_WrTagMask:
        ch.tag_mask = value;
        mfc_update_tag_status(ctx);
        return 0;
    case PS_MFC_WrTagUpdate: {
        if (value > PS_MFC_TAG_UPDATE_ALL)
            return raise_exception(ctx, pc, PS_SPU_EXC_CHANNEL, channel, "MFC_WrTagUpdate with an invalid request");
        // RPCS3 set_ch_value(MFC_WrTagUpdate)
        const u32 completed = mfc_completed(ctx);
        if (value == PS_MFC_TAG_UPDATE_IMMEDIATE || (value == PS_MFC_TAG_UPDATE_ANY && completed) ||
            (value == PS_MFC_TAG_UPDATE_ALL && completed == ch.tag_mask)) {
            ch.tag_update = 0;
            ch.tag_stat = completed;
            ch.tag_stat_count = 1;
        } else {
            ch.tag_update = value;
        }
        return 0;
    }
    case PS_MFC_WrListStallAck:
        lock.unlock();
        return mfc_stall_ack(ctx, pc, value & 0x1F);
    case PS_MFC_WrMSSyncReq:
        // Every transfer has completed when it was issued, so the synchronization is immediate.
        set_events(ctx, PS_SPU_EVENT_MS);
        return 0;
    case PS_SPU_WrDec:
        collect_events(ctx, PS_SPU_EVENT_TM); // keep an event that already happened
        ch.dec_start = ps_spu_timebase();
        ch.dec_value = value;
        ch.dec_frozen = 0;
        return 0;
    case PS_SPU_WrEventMask:
        collect_events(ctx, value | ch.event_mask);
        ch.event_mask = value;
        if (ch.event_stat & ch.event_mask) ch.event_count = 1;
        if (ctx->interrupts_enabled && ch.event_count)
            __atomic_fetch_or(&ctx->attention, PS_SPU_ATTN_INTERRUPT, __ATOMIC_RELEASE);
        return 0;
    case PS_SPU_WrEventAck: {
        collect_events(ctx, value | ch.event_mask);
        ch.event_stat &= ~value;
        // RPCS3: acknowledging TM while it is masked out stops the decrementer at its current value.
        if ((value & PS_SPU_EVENT_TM) && !(ch.event_mask & PS_SPU_EVENT_TM) && !ch.dec_frozen) {
            ch.dec_value = dec_now(ctx);
            ch.dec_frozen = 1;
        }
        if (ch.event_stat & ch.event_mask) ch.event_count = 1;
        if (ctx->interrupts_enabled && ch.event_count)
            __atomic_fetch_or(&ctx->attention, PS_SPU_ATTN_INTERRUPT, __ATOMIC_RELEASE);
        return 0;
    }
    case PS_SPU_WrOutMbox:
        if (wait_until(ctx, lock, pc, [&] { return ch.out_mbox_count == 0; })) return 1;
        ch.out_mbox = value;
        ch.out_mbox_count = 1;
        notify(ctx);
        return 0;
    case PS_SPU_WrOutIntrMbox: {
        const ps_spu_intr_mbox_handler own = ctx->hooks ? ctx->hooks->intr_mbox : nullptr;
        const bool plain = ctx->hooks && ctx->hooks->plain_intr_mbox;
        if (const ps_spu_intr_mbox_handler handler = own ? own : plain ? nullptr : g_intr_handler.load()) {
            lock.unlock();
            HostFpScope fp;
            if (handler(ctx, value)) return requested_stop(ctx, pc);
            return 0;
        }
        if (wait_until(ctx, lock, pc, [&] { return ch.out_intr_mbox_count == 0; })) return 1;
        ch.out_intr_mbox = value;
        ch.out_intr_mbox_count = 1;
        notify(ctx);
        if (ctx->hooks && ctx->hooks->intr_mbox_written) {
            lock.unlock();
            HostFpScope fp;
            ctx->hooks->intr_mbox_written(ctx);
        }
        return 0;
    }
    case PS_SPU_Set_Bkmk_Tag:
    case PS_SPU_PM_Start_Ev:
    case PS_SPU_PM_Stop_Ev: return 0;
    default: return raise_exception(ctx, pc, PS_SPU_EXC_CHANNEL, channel, "wrch of an invalid channel");
    }
}

int ps_spu_stop(SPUContext* ctx, uint32_t pc, uint32_t code)
{
    const ps_spu_stop_handler own = ctx->hooks ? ctx->hooks->stop : nullptr;
    if (const ps_spu_stop_handler handler = own ? own : g_stop_handler.load()) {
        HostFpScope fp;
        if (handler(ctx, code)) return 0;
    }
    ctx->pc = (pc + 4) & PS_SPU_PC_MASK;
    ctx->stop_reason = PS_SPU_STOP_SIGNAL;
    ctx->stop_code = code;
    return 1;
}

int ps_spu_halt(SPUContext* ctx, uint32_t pc)
{
    PS_LOG_ERROR("spu", "SPU {} halted at 0x{:05x}", ctx->id, pc);
    ctx->pc = pc & PS_SPU_PC_MASK;
    ctx->stop_reason = PS_SPU_STOP_HALT;
    ctx->stop_code = 0;
    return 1;
}

int ps_spu_illegal(SPUContext* ctx, uint32_t pc, uint32_t insn)
{
    return raise_exception(ctx, pc, PS_SPU_EXC_ILLEGAL, insn, "illegal or unsupported instruction");
}

int ps_spu_events_pending(SPUContext* ctx)
{
    ChannelLock lock(ctx);
    collect_events(ctx, PS_SPU_EVENT_ALL);
    return ctx->ch.event_count ? 1 : 0;
}

int ps_spu_set_interrupts(SPUContext* ctx, int enable)
{
    ChannelLock lock(ctx);
    ctx->interrupts_enabled = enable ? 1 : 0;
    if (!enable) return 0;
    collect_events(ctx, ctx->ch.event_mask);
    if (!ctx->ch.event_count) return 0;
    take_interrupt(ctx);
    return 1;
}

// ---- PPU side ----

void ps_spu_write_in_mbox(SPUContext* ctx, uint32_t value)
{
    ChannelLock lock(ctx);
    ps_spu_channels& ch = ctx->ch;
    if (ch.in_mbox_count < 4)
        ch.in_mbox[ch.in_mbox_count++] = value;
    else
        ch.in_mbox[3] = value; // full: the newest entry is replaced (RPCS3 spu_channel_4_t::push)
    set_events(ctx, PS_SPU_EVENT_MB);
    notify(ctx);
}

uint32_t ps_spu_in_mbox_count(SPUContext* ctx)
{
    ChannelLock lock(ctx);
    return ctx->ch.in_mbox_count;
}

int ps_spu_read_out_mbox(SPUContext* ctx, uint32_t* value)
{
    ChannelLock lock(ctx);
    if (!ctx->ch.out_mbox_count) return 0;
    *value = ctx->ch.out_mbox;
    ctx->ch.out_mbox_count = 0;
    set_events(ctx, PS_SPU_EVENT_LE);
    notify(ctx);
    return 1;
}

int ps_spu_read_out_intr_mbox(SPUContext* ctx, uint32_t* value)
{
    ChannelLock lock(ctx);
    if (!ctx->ch.out_intr_mbox_count) return 0;
    *value = ctx->ch.out_intr_mbox;
    ctx->ch.out_intr_mbox_count = 0;
    set_events(ctx, PS_SPU_EVENT_ME);
    notify(ctx);
    return 1;
}

int ps_spu_wait_out_mbox(SPUContext* ctx, uint32_t* value, uint64_t timeout_ns)
{
    ChannelLock lock(ctx);
    ps_spu_channels& ch = ctx->ch;
    if (ctx->host && !ch.out_mbox_count) {
        const auto ready = [&] { return ch.out_mbox_count != 0; };
        if (timeout_ns)
            ctx->host->cv.wait_for(lock.lock(), std::chrono::nanoseconds(timeout_ns), ready);
        else
            ctx->host->cv.wait(lock.lock(), ready);
    }
    if (!ch.out_mbox_count) return 0;
    *value = ch.out_mbox;
    ch.out_mbox_count = 0;
    set_events(ctx, PS_SPU_EVENT_LE);
    notify(ctx);
    return 1;
}

void ps_spu_write_signal(SPUContext* ctx, uint32_t which, uint32_t value)
{
    ChannelLock lock(ctx);
    push_signal(ctx, which == 2 ? 1 : 0, value);
    notify(ctx); // an SPU blocked reading the register (set_events wakes only masked events' waiters)
}

void ps_spu_set_signal_or_mode(SPUContext* ctx, uint32_t which, int or_mode)
{
    ChannelLock lock(ctx);
    const u32 bit = which == 2 ? 2u : 1u;
    if (or_mode)
        ctx->ch.snr_or_mode |= bit;
    else
        ctx->ch.snr_or_mode &= ~bit;
}

void ps_spu_request_stop(SPUContext* ctx)
{
    if (ctx->host) {
        ChannelLock lock(ctx);
        ctx->host->stop_requested.store(true, std::memory_order_release);
        __atomic_fetch_or(&ctx->attention, PS_SPU_ATTN_STOP, __ATOMIC_RELEASE);
        ctx->host->cv.notify_all();
    } else {
        __atomic_fetch_or(&ctx->attention, PS_SPU_ATTN_STOP, __ATOMIC_RELEASE);
    }
}

void ps_spu_clear_stop(SPUContext* ctx)
{
    if (ctx->host) ctx->host->stop_requested.store(false, std::memory_order_release);
    __atomic_fetch_and(&ctx->attention, ~PS_SPU_ATTN_STOP, __ATOMIC_RELEASE);
}

uint32_t ps_spu_status(SPUContext* ctx) { return ctx->host ? ctx->host->status.load(std::memory_order_acquire) : 0; }

} // extern "C"
