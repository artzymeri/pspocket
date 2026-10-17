#pragma once

// Runtime-private definitions shared by the runtime/spu sources.

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "pspocket/core/types.hpp"
#include "pspocket/spu/context.h"
#include "pspocket/spu/runtime.h"

// An image the context runs with no lifted program (D-233): looked up again when programs are registered during the
// session (pspocket::spu::set_pending_home). The SPU's own thread only.
struct ps_spu_pending_home {
    pspocket::u64 hash = 0; // the image's ps_spu_hash64 when it was loaded
    pspocket::u32 ls_start = 0, size = 0;
    bool relocatable = false, armed = false;
    pspocket::u64 checked = 0; // pspocket::spu::registry_additions() at the last lookup
};

struct ps_spu_host {
    std::mutex mutex;
    std::condition_variable cv; // channel state changed (SPU waits for data/space, PPU waits for the outbound mailbox)
    std::atomic<bool> stop_requested{false};
    std::atomic<pspocket::u32> status{PS_SPU_STATUS_STOPPED};
    ps_spu_pending_home pending;
    // The last DMA it made (D-1091, for a stall's report): written by its own thread, read racily.
    std::atomic<pspocket::u32> last_cmd{0}, last_ea{0}, last_size{0}, last_pc{0};
    std::atomic<pspocket::u64> dmas{0};
};

namespace pspocket::spu::detail {

// Holds the context's lock when it has a host (single-thread contexts need none).
class ChannelLock {
public:
    explicit ChannelLock(SPUContext* ctx) : host_(ctx->host)
    {
        if (host_) lock_ = std::unique_lock<std::mutex>(host_->mutex);
    }
    std::unique_lock<std::mutex>& lock() { return lock_; }
    void unlock()
    {
        if (lock_.owns_lock()) lock_.unlock();
    }
    void relock()
    {
        if (host_ && !lock_.owns_lock()) lock_.lock();
    }

private:
    ps_spu_host* host_;
    std::unique_lock<std::mutex> lock_;
};

inline void notify(SPUContext* ctx)
{
    if (ctx->host) ctx->host->cv.notify_all();
}

// Stops execution with an exception at pc (logs it). Returns 1 (the "stop" result of runtime calls).
int raise_exception(SPUContext* ctx, u32 pc, u32 exception, u32 detail, const char* what);

// Event bookkeeping (lock held when the context has a host).
void set_events(SPUContext* ctx, u32 bits);
// Evaluates the events that are computed on demand (reservation lost, decrementer) for the bits in mask_hint.
void collect_events(SPUContext* ctx, u32 mask_hint);
// True when the reservation's line changed since GETLLAR (version or bytes).
bool reservation_lost(const SPUContext* ctx);

// MFC_Cmd with the parameters in ctx->ch (mfc.cpp). Returns 1 when execution must stop (exception).
int mfc_command(SPUContext* ctx, u32 pc, u32 cmd);
// MFC_WrListStallAck (mfc.cpp).
int mfc_stall_ack(SPUContext* ctx, u32 pc, u32 tag);
// Re-evaluates a pending conditional tag-status update (mfc.cpp).
void mfc_update_tag_status(SPUContext* ctx);
inline u32 mfc_completed(const SPUContext* ctx) { return ctx->ch.tag_mask & ~ctx->ch.queue_fence; }

// Takes a pending interrupt (interrupts enabled and an enabled event pending): SRR0 = pc, pc = handler.
void check_interrupt(SPUContext* ctx);
// For pc outside the current program's code: makes the home program (back from a module) or the one the resolver
// (ps_spu_set_resolver) names current, and returns true when it has lifted code at pc (runtime.cpp).
bool resolve_program(SPUContext* ctx, u32 pc);
// Whether program p, running reloc bytes past where it was lifted, holds pc.
bool holds(const ps_spu_program* p, u32 reloc, u32 pc);
// The reloc of program p with its code at code_start (0 unless relocatable).
u32 reloc_for(const ps_spu_program* p, u32 code_start);
// Makes p (at reloc) the current program; home stays.
void set_current(SPUContext* ctx, const ps_spu_program* p, u32 reloc);
// Forgets the context's pending home (an attach, a reset).
inline void disarm_pending(SPUContext* ctx)
{
    if (ctx->host) ctx->host->pending.armed = false;
}
inline bool pending_armed(const SPUContext* ctx) { return ctx->host && ctx->host->pending.armed; }
// The GET observer (ps_spu_set_get_observer).
void observe_get(SPUContext* ctx, u32 lsa, u32 eal, u32 size);

// Run-state bookkeeping for ps_spu_status.
void set_status(SPUContext* ctx, u32 status);

// FP environment switch around calls into lv2 hooks.
struct HostFpScope {
    HostFpScope();
    ~HostFpScope();
    HostFpScope(const HostFpScope&) = delete;
    HostFpScope& operator=(const HostFpScope&) = delete;

private:
    // Used on AArch64 only (the FPCR switch); other hosts keep the host FP environment throughout.
    [[maybe_unused]] u64 saved_ = 0;
    [[maybe_unused]] bool restore_ = false;
};

} // namespace pspocket::spu::detail
