#pragma once

// SPU threads: one host thread per SPU thread (plan section 6, "Threading"), created with an 8 MB stack at
// userInitiated QoS (D-015: lifted code nests host calls as deep as the guest nests calls). The thread runs
// ps_spu_run on its context, which enters the SPU floating-point environment (pspocket/spu/fp.h); stop-and-signal
// codes reach the process-wide stop handler first (pspocket/spu/runtime.h), and when execution stops the exit
// callback gets the context and the stop reason (the lv2 layer maps them to sys_spu_thread exit, group exit and
// exception events).

#include <atomic>
#include <functional>
#include <string>

#include "pspocket/core/types.hpp"
#include "pspocket/spu/context.h"

// For lv2's SPU threads (D-167), kept out of runtime.h, whose text is part of the game library ABI (D-152).
extern "C" {
/* An SPU thread as sys_spu_thread_group_start leaves it before setting its arguments and entry (RPCS3
 * spu_thread::cpu_init): registers zero but $1 = 0x3FFF0, FPSCR, SRR0, the MFC (parameters, queue, tag groups, list
 * stalls, atomic status), the outbound mailboxes, events, interrupts and the reservation cleared, the decrementer
 * restarted at dec_value; the inbound mailbox, the signal notification registers and their OR modes kept. The local
 * store, membase, host, identity, hooks and group stay; the program is detached. */
void ps_spu_thread_reset(SPUContext* ctx, uint32_t dec_value);
/* Empties the inbound mailbox (RPCS3: an SPU thread's, when it stops). */
void ps_spu_clear_in_mbox(SPUContext* ctx);
}

namespace pspocket::spu {

class Thread {
public:
    static constexpr usize kStackSize = 8u << 20;

    // Called on the SPU thread when ps_spu_run returns (stop_reason = ctx.stop_reason). Without a callback,
    // exceptions and halts abort the process (CLAUDE.md rule 5).
    using ExitCallback = std::function<void(SPUContext& ctx, u32 stop_reason)>;

    // The context must outlive the thread. A host part (lock, run state) is attached if it has none.
    explicit Thread(SPUContext& ctx, std::string name = "SPU");
    ~Thread(); // requests a stop and joins
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    // Starts executing at ctx.pc. Returns false if the host thread could not be created or one is running.
    bool start(ExitCallback on_exit = {});
    // Asks the SPU to stop (ps_spu_request_stop); join() waits for it.
    void request_stop();
    void join();
    bool running() const { return running_.load(std::memory_order_acquire); }
    SPUContext& context() { return ctx_; }

private:
    static void* entry(void* arg);
    void body();

    SPUContext& ctx_;
    std::string name_;
    ExitCallback on_exit_;
    std::atomic<bool> running_{false};
    bool joinable_ = false;
    void* handle_ = nullptr; // pthread_t storage
};

} // namespace pspocket::spu
