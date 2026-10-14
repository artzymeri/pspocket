// The native SPURS scheduler (D-037 stage 3b, D-050): one host worker per SPU of a SPURS instance. A worker selects
// a workload with the SPURS kernel's rules, runs the workload's policy natively, and sleeps while nothing is ready.
// References: RPCS3 rpcs3/Emu/Cell/Modules/cellSpursSpu.cpp spursKernel1SelectWorkload (selection rules) and
// cellSpurs.cpp _cellSpursSendSignal; Sony's libsre (fw 4.92, read as the reference, D-046) for what the kernel keeps
// in local store, how idle SPUs wait, and how SPU code raises events.
//
// Idle workers wait for any change to CellSpurs line 0 (ps_res_wait_change): that is what the SPURS kernel's idle loop
// waits on (GETLLAR line 0 and the lock-line-lost event), and every way work arrives changes it through the
// reservation system: ready counts and workload signals from the PPU side (HLE reservation_op), from SPU task code
// (PUTLLC) and from the policy itself. cellSpursWakeUp notifies explicitly. A selection that takes nothing stores
// nothing, so an idle worker's own look at the line doesn't end its wait.
//
// SPURS2 instances (32 workloads, cellSpursInitializeWithAttribute2) select with kernel 2's rules (select2, D-171).
//
// Deviations from the SPURS kernel: the system service workload never runs (its jobs, activating workloads and
// processing their state changes, are read from the guest state directly, and its message bit is just acknowledged);
// pending contention (switch requests from polls) is not tracked.

#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <thread>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/core/reservation.h"
#include "pspocket/core/timebase.h"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/spu/fp.h"
#include "pspocket/spu/registry.hpp"
#include "pspocket/spu/runtime.h"
#include "spurs_native.hpp"

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace pspocket::hle::spurs {
namespace {

constexpr std::string_view kSchedChan = "spurs";
constexpr u64 kIdleWaitNs = 100'000'000; // re-check for a stop request this often while idle
// A game's own policy module that keeps finding no work (D-103): after this many empty polls in a row, waits that grow
// from the first to the last value.
constexpr u32 kIdleStreak = 64;
constexpr u64 kMinBackoffNs = 5'000;
constexpr u64 kMaxBackoffNs = 100'000;
constexpr usize kWorkerStack = 8u << 20;  // as SPU threads (D-015)
constexpr u32 CELL_SPURS_MODULE_POLL_STATUS_READYCOUNT = 1, CELL_SPURS_MODULE_POLL_STATUS_SIGNAL = 2,
              CELL_SPURS_MODULE_POLL_STATUS_FLAG = 4;
constexpr u64 SYS_SPU_THREAD_EVENT_USER_KEY = 0xFFFFFFFF53505501ull;

// Worker contexts handle their own stops: every stop returns to the worker (the syscall trap, and anything else,
// which the worker reports).
int worker_stop(SPUContext*, uint32_t) { return 0; }

#if defined(__APPLE__)
// The workers' QoS class (D-950): user initiated, or PSPOCKET_SPU_QOS for all of them (interactive, initiated, default,
// utility), PSPOCKET_SPU_QOS_SOLO for an instance of one SPU (GTA V's sound mixer). Measuring aids for the phone,
// where six cores of two kinds run them.
qos_class_t qos_named(const char* v)
{
    const std::string_view s = v ? v : "";
    return s == "interactive" ? QOS_CLASS_USER_INTERACTIVE
         : s == "default"     ? QOS_CLASS_DEFAULT
         : s == "utility"     ? QOS_CLASS_UTILITY
                              : QOS_CLASS_USER_INITIATED;
}

qos_class_t worker_qos(u32 nspus)
{
    const char* v = std::getenv("PSPOCKET_SPU_QOS");
    if (const char* solo = std::getenv("PSPOCKET_SPU_QOS_SOLO"); solo && nspus == 1) v = solo;
    return qos_named(v);
}

const char* qos_name(int q)
{
    return q == QOS_CLASS_USER_INTERACTIVE ? "interactive"
         : q == QOS_CLASS_USER_INITIATED   ? "initiated"
         : q == QOS_CLASS_DEFAULT          ? "default"
         : q == QOS_CLASS_UTILITY          ? "utility"
                                           : "other";
}
#endif

// sys_event_flag_set_bit (code 128, answers in the inbound mailbox) and sys_event_flag_set_bit_impatient (192, does
// not): bit `value & 0xFFFFFF` of the lv2 event flag whose ID is in the outbound mailbox (RPCS3 SPUThread.cpp
// set_ch_value, SPU_WrOutIntrMbox).
int event_flag_set_bit(Worker& w, SPUContext* ctx, u32 value)
{
    const u32 code = value >> 24, bit = value & 0xFFFFFF;
    if (!ctx->ch.out_mbox_count) {
        PS_FATAL(kSchedChan, "SPURS {:#x} SPU {}: sys_event_flag_set_bit{}(bit {}) with an empty outbound mailbox at "
            "0x{:05x}", w.sched.spurs, w.spu_num, code == 192 ? "_impatient" : "", bit, ctx->pc);
    }
    const u32 id = ctx->ch.out_mbox;
    ctx->ch.out_mbox_count = 0;
    if (bit > 63) {
        PS_FATAL(kSchedChan, "SPURS {:#x} SPU {}: sys_event_flag_set_bit{}(event flag {:#x}, bit {}) at 0x{:05x}: no such "
            "bit", w.sched.spurs, w.spu_num, code == 192 ? "_impatient" : "", id, bit, ctx->pc);
    }
    lv2::Result r = lv2::err(lv2::CELL_ESRCH);
    if (const auto flag = w.sched.kernel.event_flag(id)) {
        flag->set(u64{1} << bit);
        r = lv2::CELL_OK;
    }
    PS_LOG_DEBUG(kSchedChan, "SPURS {:#x} SPU {}: sys_event_flag_set_bit{}(event flag {:#x}, bit {}) -> {}", w.sched.spurs,
        w.spu_num, code == 192 ? "_impatient" : "", id, bit, lv2::error_name(r));
    if (r != lv2::CELL_OK) {
        PS_LOG_WARN(kSchedChan, "SPURS {:#x} SPU {}: event flag {:#x} bit {} not set: {}", w.sched.spurs, w.spu_num, id, bit,
            lv2::error_name(r));
    }
    if (code == 128) ps_spu_write_in_mbox(ctx, static_cast<u32>(r));
    return 0;
}

// sys_spu_thread_send_event / throw_event from SPU code (codes 0..127 in the interrupt mailbox, the data word in the
// outbound mailbox), through the instance's SPU port map (D-040), as RPCS3 SPUThread.cpp set_ch_value
// (SPU_WrOutIntrMbox). send_event (0..63) also answers in the inbound mailbox; throw_event (64..127) does not. Codes
// 128 and 192 set event flag bits.
int worker_intr_mbox(SPUContext* ctx, uint32_t value)
{
    Worker& w = *static_cast<Worker*>(ctx->runtime);
    const u32 code = value >> 24;
    if (code == 128 || code == 192) return event_flag_set_bit(w, ctx, value);
    if (code >= 128) {
        PS_FATAL(kSchedChan, "SPURS {:#x} SPU {}: interrupt mailbox code {:#x} (value {:#010x}) at 0x{:05x} is not "
            "implemented", w.sched.spurs, w.spu_num, code, value, ctx->pc);
    }
    const u8 port = static_cast<u8>(code & 63);
    if (!ctx->ch.out_mbox_count) {
        PS_FATAL(kSchedChan, "SPURS {:#x} SPU {}: sys_spu_thread_{}_event(port {}) with an empty outbound mailbox",
            w.sched.spurs, w.spu_num, code < 64 ? "send" : "throw", port);
    }
    const u32 data = ctx->ch.out_mbox;
    ctx->ch.out_mbox_count = 0;
    u32 queue_id = 0;
    {
        std::lock_guard lock(w.sched.host_mutex);
        if (const auto it = w.sched.inst.spu_ports.find(port); it != w.sched.inst.spu_ports.end()) queue_id = it->second;
    }
    lv2::Result r = lv2::err(lv2::CELL_ENOTCONN);
    if (const auto queue = queue_id ? w.sched.kernel.event_queue(queue_id) : nullptr) {
        // data1: the sender's SPU thread ID; a native SPURS has no SPU threads, so the worker's number stands in.
        r = queue->send(lv2::Event{SYS_SPU_THREAD_EVENT_USER_KEY, w.spu_num, (u64{port} << 32) | (value & 0xFFFFFF),
            data});
    }
    PS_LOG_DEBUG(kSchedChan, "SPURS {:#x} SPU {}: {} event on port {} ({:#x}, {:#x}) -> {}", w.sched.spurs, w.spu_num,
        code < 64 ? "send" : "throw", port, value & 0xFFFFFF, data, lv2::error_name(r));
    if (r != lv2::CELL_OK && r != lv2::err(lv2::CELL_EBUSY)) {
        PS_LOG_WARN(kSchedChan, "SPURS {:#x} SPU {}: event on port {} not delivered: {}", w.sched.spurs, w.spu_num, port,
            lv2::error_name(r));
    }
    if (code < 64) ps_spu_write_in_mbox(ctx, static_cast<u32>(r));
    return 0;
}

constexpr ps_spu_hooks kWorkerHooks{worker_stop, worker_intr_mbox, 0, nullptr};

// A worker left workload `wid` (its count of workers inside went down by one, to `now`). The last one to leave a
// taskset that is removable completes the taskset's shutdown, as libsre's shutdown-completion hook runs once the SPUs
// left it (D-914); cellSpursShutdownWorkload does it itself when no worker was inside.
void left_workload(Scheduler& sched, u32 wid, u32 now)
{
    if (now != 0) return;
    if (__atomic_load_n(sched.base + sched.spurs + wkl_state(wid), __ATOMIC_SEQ_CST) != SPURS_WKL_STATE_REMOVABLE) return;
    u32 taskset = 0;
    {
        std::lock_guard lock(sched.host_mutex);
        const auto it = sched.inst.workloads.find(wid);
        if (it == sched.inst.workloads.end() || it->second.kind != Workload::Kind::Taskset) return;
        taskset = it->second.arg;
    }
    taskset_shutdown_complete(sched.kernel, sched.base, taskset);
}

} // namespace

// ------------------------------------------------------------------------------------------------- worker

Worker::Worker(Scheduler& scheduler, u32 num) : sched(scheduler), spu_num(num)
{
    ls = static_cast<u8*>(std::aligned_alloc(128, ls::size));
    std::memset(ls, 0, ls::size);
    ps_spu_context_init(&ctx, ls, sched.base);
    ctx.id = 0x0F000000u | spu_num;
    ctx.index = spu_num;
    ctx.runtime = this;
    ctx.hooks = &kWorkerHooks;
    ps_spu_host_attach(&ctx);

    // The kernel context fields a task can read, as SPURS kernel 1 sets them up (libsre; RPCS3 spursKernelEntry).
    ps_write64(ls, ls::k_spurs, sched.spurs);
    ps_write32(ls, ls::k_spu_num, spu_num);
    ps_write32(ls, ls::k_dma_tag, 31);
    ps_write32(ls, ls::k_wkl_unique, kIdleWid);
    ps_write32(ls, ls::k_wkl_id, kIdleWid);
    ps_write32(ls, ls::k_exit_to_kernel, sched.second ? kKernel2Exit : kKernelExit);
    ps_write32(ls, ls::k_select, kKernelSelect);
    // The kernel's entry points are `stop`s the worker takes (no Sony kernel code runs): kernel 1's return from a module
    // and exit, or kernel 2's (one address).
    ps_write32(ls, kKernelSelect, kKernelSelectStop);
    if (sched.second) {
        ps_write32(ls, kKernel2Exit, kKernelExitStop);
    } else {
        ps_write32(ls, kKernelReturn, kKernelExitStop);
        ps_write32(ls, kKernelExit, kKernelExitStop);
    }
}

Worker::~Worker()
{
    join();
    ps_spu_host_detach(&ctx);
    std::free(ls);
    delete static_cast<pthread_t*>(thread_);
}

bool Worker::start()
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kWorkerStack);
#if defined(__APPLE__)
    pthread_attr_set_qos_class_np(&attr, worker_qos(sched.inst.nspus), 0);
#endif
    if (!thread_) thread_ = new pthread_t{};
    const int rc = pthread_create(static_cast<pthread_t*>(thread_), &attr, &Worker::entry, this);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        PS_LOG_ERROR(kSchedChan, "SPURS {:#x}: cannot create the host thread of SPU {} (error {})", sched.spurs, spu_num,
            rc);
        return false;
    }
    joinable_ = true;
    return true;
}

void Worker::request_stop() { ps_spu_request_stop(&ctx); }

void Worker::join()
{
    if (!joinable_) return;
    pthread_join(*static_cast<pthread_t*>(thread_), nullptr);
    joinable_ = false;
}

void* Worker::entry(void* arg)
{
    static_cast<Worker*>(arg)->body();
    return nullptr;
}

void Worker::body()
{
    const std::string name = std::format("SPURS {:x} SPU {}", sched.spurs, spu_num);
#if defined(__APPLE__)
    pthread_setname_np(name.c_str());
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#endif
    // The SPU's floating-point mode (pspocket/spu/fp.h) for the worker's whole life: every ps_spu_run would otherwise
    // set the host's FP control register and restore it, thousands of times a second (run 050: about 3.5% of the
    // workers' time). The scheduler and policies around the SPU code do no floating point.
    const u64 fp_env = ps_spu_fpenv_enter();
    // The host clock of one multiply (D-241): steady_clock's reads were 2% of the workers' busy time (run 221).
    bool in_b = false; // the half of PSPOCKET_SPU_AB this worker's QoS is set for (D-950)
#if defined(__APPLE__)
    const int base_qos = sched.ab_period_ns ? sched.qos_a : static_cast<int>(worker_qos(sched.inst.nspus));
    int qos_now = base_qos; // the class this thread has now (D-973)
#endif
    while (!sched.stopping.load(std::memory_order_acquire)) {
        if (sched.ab_period_ns) {
            const u64 now = ps_host_now_ns();
            const bool b = sched.in_b(now);
#if defined(__APPLE__)
            if (b != in_b && sched.qos_a != sched.qos_b) {
                pthread_set_qos_class_self_np(static_cast<qos_class_t>(b ? sched.qos_b : sched.qos_a), 0);
                qos_now = b ? sched.qos_b : sched.qos_a;
            }
#endif
            if (b != in_b) spu::set_translation_b(b); // the B translation of the SPU programs, when built in (D-962)
            in_b = b;
        }
        // The version is read before selecting, so a change made while selecting ends the wait at once.
        const u64 version = ps_res_version(sched.spurs);
        const Selection sel = select(false);
        if (spu_num == 0) sched.report_time(ps_host_now_ns());
        if (sel.wid >= CELL_SPURS_MAX_WORKLOAD2) {
            const u64 t0 = ps_host_now_ns();
            ps_res_wait_change(sched.spurs, version, kIdleWaitNs);
            const u64 waited = ps_host_now_ns() - t0;
            ns_wait += waited;
            sched.wait_ns.fetch_add(waited, std::memory_order_relaxed);
            continue;
        }
#if defined(__APPLE__)
        // A workload of one SPU at a time on the QoS class asked for, the others on the worker's own (D-973).
        if (sched.qos_serial) {
            // With PSPOCKET_SPU_AB, only in its A half: the B half measures without it in the same session.
            const bool serial = sel.serial && !(sched.ab_period_ns && in_b);
            const int want = serial ? sched.qos_serial : (in_b ? sched.qos_b : base_qos);
            if (want != qos_now) {
                pthread_set_qos_class_self_np(static_cast<qos_class_t>(want), 0);
                qos_now = want;
            }
        }
#endif
        const u64 writes = ctx.mem_writes;
        const u64 t0 = ps_host_now_ns();
        // The selection read the workload's state before its update of the line: a shutdown and a join may have come
        // in between, and the game may be freeing the workload. The worker counts itself inside, then reads the state
        // again (both sequentially consistent, as the join's read of the state and then of the count): either it sees
        // the shutdown and leaves, or the join sees it inside and waits (D-136).
        sched.inside[sel.wid].fetch_add(1, std::memory_order_seq_cst);
        if (__atomic_load_n(sched.base + sched.spurs + wkl_state(sel.wid), __ATOMIC_SEQ_CST) != SPURS_WKL_STATE_RUNNABLE) {
            left_workload(sched, sel.wid, sched.inside[sel.wid].fetch_sub(1, std::memory_order_seq_cst) - 1);
            continue;
        }
        // The DMA log's tag for what the workload's policy writes (a job chain tags its jobs' writes, D-138).
        ps_spu_set_dma_tag(0xFF00000000000000ull | (u64{sel.wid} << 48) | sched.spurs);
        const bool custom = run_workload(sel);
        left_workload(sched, sel.wid, sched.inside[sel.wid].fetch_sub(1, std::memory_order_seq_cst) - 1);
        const u64 dt = ps_host_now_ns() - t0;
        if (custom && ctx.mem_writes == writes) {
            sched.idle_poll_ns.fetch_add(dt, std::memory_order_relaxed);
            ns_idle_polls += dt;
            idle_polls++;
            // GTA V's policy module polled two million times a second on each of four SPUs (run 074): the PS3's SPUs
            // spin, but here each would take a core the PPU threads need. After a run of empty polls the worker sleeps,
            // a little longer each time up to 100 us, the longest new work then waits. Waiting for the line the module
            // polls to change woke it at once: the other SPUs take jobs from the same queue.
            if (++idle_streak >= kIdleStreak) {
                backoff_ns = backoff_ns ? std::min(backoff_ns * 2, kMaxBackoffNs) : kMinBackoffNs;
                const u64 w0 = ps_host_now_ns();
                ps_host_sleep_ns(backoff_ns);
                const u64 slept = ps_host_now_ns() - w0;
                ns_wait += slept;
                sched.wait_ns.fetch_add(slept, std::memory_order_relaxed);
            }
        } else {
            sched.workload_ns[sel.wid].fetch_add(dt, std::memory_order_relaxed);
            ns_work += dt;
            idle_streak = 0;
            backoff_ns = 0;
#if defined(__APPLE__)
            size_t cpu = 0;
            if (pthread_cpu_number_np(&cpu) == 0 && cpu < sched.cpu_ns.size())
                sched.cpu_ns[cpu].fetch_add(dt, std::memory_order_relaxed);
#endif
        }
    }
    release();
    PS_LOG_INFO(kSchedChan, "SPURS {:#x} SPU {}: {:.1f} s running workloads, {:.1f} s in {} runs of a policy module that "
        "changed nothing, {:.1f} s waiting", sched.spurs, spu_num, ns_work / 1e9, ns_idle_polls / 1e9, idle_polls,
        ns_wait / 1e9);
    ps_spu_fpenv_leave(fp_env);
}

// Every 5 s, from the first worker: the SPU time each workload took, by its name, and what was idle (D-131). The
// iPhone's SPU workers are its biggest load; this says which of the game's systems the time went to.
void Scheduler::report_time(u64 now_ns)
{
    constexpr u64 kPeriodNs = 5'000'000'000ull;
    if (!last_report_ns) last_report_ns = now_ns;
    if (now_ns - last_report_ns < kPeriodNs) return;
    const double secs = static_cast<double>(now_ns - last_report_ns) / 1e9;
    last_report_ns = now_ns;
    std::array<std::pair<u64, u32>, CELL_SPURS_MAX_WORKLOAD2> taken{};
    for (u32 i = 0; i < CELL_SPURS_MAX_WORKLOAD2; i++) taken[i] = {workload_ns[i].exchange(0, std::memory_order_relaxed), i};
    std::sort(taken.begin(), taken.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string parts;
    {
        std::lock_guard lock(host_mutex);
        for (const auto& [t, wid] : taken) {
            if (t < 5'000'000) continue; // under 5 ms
            const auto it = inst.workloads.find(wid);
            const std::string name = it != inst.workloads.end() && !it->second.name.empty() ? it->second.name
                                                                                           : std::format("workload {}", wid);
            parts += std::format("{}{} {:.2f} s", parts.empty() ? "" : ", ", name, static_cast<double>(t) / 1e9);
        }
    }
    // The workloads' time by host CPU (D-950), as shares.
    std::array<u64, 16> by_cpu{};
    u64 all_cpus = 0;
    for (usize i = 0; i < by_cpu.size(); i++) all_cpus += by_cpu[i] = cpu_ns[i].exchange(0, std::memory_order_relaxed);
    std::string cpus;
    for (usize i = 0; i < by_cpu.size() && all_cpus; i++) {
        if (by_cpu[i] * 100 >= all_cpus) {
            cpus += std::format("{}{} {:.0f}%", cpus.empty() ? "" : ", ", i,
                100.0 * static_cast<double>(by_cpu[i]) / static_cast<double>(all_cpus));
        }
    }
    std::string half;
    if (ab_period_ns) half = std::format("; half {}", in_b(now_ns) ? "B" : "A");
    PS_LOG_INFO(kSchedChan, "SPURS {:#x}, SPU time in {:.1f} s on {} SPUs: {}; empty policy polls {:.2f} s; waiting "
        "for work {:.2f} s{}{}", spurs, secs, workers_.size(), parts.empty() ? "nothing" : parts,
        static_cast<double>(idle_poll_ns.exchange(0, std::memory_order_relaxed)) / 1e9,
        static_cast<double>(wait_ns.exchange(0, std::memory_order_relaxed)) / 1e9,
        cpus.empty() ? "" : "; by host CPU: " + cpus, half);
}

// Gives this SPU's contention back (the worker stops). SPURS2's packed share subtracts byte by byte: this SPU's nibble
// is part of the byte's count, so nothing borrows across the nibbles.
void Worker::release()
{
    if (current_wid >= CELL_SPURS_MAX_WORKLOAD2) return;
    reservation_op(sched.base, sched.spurs, [this](u8* line) {
        for (u32 i = 0; i < CELL_SPURS_MAX_WORKLOAD; i++) {
            const u8 c = ps_read8(line, wkl_off::currentContention + i);
            ps_write8(line, wkl_off::currentContention + i, static_cast<u8>(c - loc_contention[i]));
        }
        return true;
    });
    loc_contention.fill(0);
    current_wid = kIdleWid;
}

Worker::Selection Worker::select(bool poll) { return sched.second ? select2(poll) : select1(poll); }

void Worker::update_kernel_context(const std::array<u8, 16>& priority, const std::array<u8, 16>& unique, u32 wid,
    const u8* line0)
{
    // The kernel context in local store: the CellSpurs line 0 copy (task code reads flags1 at 0x174 from it), the
    // priorities, the current workload. The line as the selection left it (D-972: not read again).
    std::memcpy(ls + ls::kernel, line0, PS_RES_LINE_SIZE);
    std::memcpy(ls + ls::k_priority, priority.data(), priority.size());
    std::memcpy(ls + ls::k_unique_ids, unique.data(), unique.size());
    // wklCurrentAddr (0x1D0) and the unique ID are the dispatch's: a custom policy module is reloaded only when the
    // address changes.
    ps_write32(ls, ls::k_wkl_id, wid);
}

// Kernel 2's selection (SPURS2: RPCS3 spursKernel2SelectWorkload, the rules of libsre's second SPU kernel): 32
// workloads, each counted in its nibble of the contention and maximum contention bytes (wid & 15's byte: wid in the low
// nibble, wid | 16 in the high one); ready counts from wklReadyCount1 and wklIdleSpuCountOrReadyCount2, signals from
// wklSignal1 and wklSignal2. A workload is a candidate when it is runnable, has a priority on this SPU, is below its
// maximum contention and has its flag, its signal or more ready count than contention; the weight is only its priority
// (16 - the game's value) and the current workload's tie-breaker, the lower ID winning a tie. As kernel 1's here, no
// pending contention is kept (spurs_scheduler.cpp's header).
Worker::Selection Worker::select2(bool poll)
{
    u8* base = sched.base;
    const u32 spurs = sched.spurs;
    // What the system service keeps in each SPU's kernel context (RPCS3 spursSysServiceActivateWorkload): the runnable
    // workloads, each one's priority on this SPU as 16 - priority (0: not on this SPU), packed as the bytes it keeps
    // (wid & 15's byte: wid low, wid | 16 high), and the unique IDs of workloads 0..15.
    std::array<u8, CELL_SPURS_MAX_WORKLOAD2> priority{};
    std::array<u8, CELL_SPURS_MAX_WORKLOAD> packed{}, unique{};
    u32 runnable = 0;
    // The PPU side changes these while the workers select (add_workload, cellSpursSetPriority, the states through
    // reservation_op): each is read atomically, a workload's state before the priority it makes valid (D-587).
    const u32 enabled = atomic_load_be32(base, spurs + wkl_off::enabled);
    const u32 unsupported = sched.unsupported.load(std::memory_order_relaxed);
    for (u32 i = 0; i < CELL_SPURS_MAX_WORKLOAD2; i++) {
        if ((enabled & (0x80000000u >> i)) && atomic_load_u8(base, spurs + wkl_state(i)) == SPURS_WKL_STATE_RUNNABLE &&
            !(unsupported & (1u << i))) {
            runnable |= 0x80000000u >> i;
        }
        const u8 p = atomic_load_u8(base, wkl_info(spurs, i) + wkl_off::info_priority + spu_num);
        priority[i] = p ? static_cast<u8>((0x10 - p) & 0x0F) : 0;
        if (i < CELL_SPURS_MAX_WORKLOAD) unique[i] = atomic_load_u8(base, wkl_info(spurs, i) + wkl_off::info_unique);
    }
    for (u32 j = 0; j < CELL_SPURS_MAX_WORKLOAD; j++) packed[j] = static_cast<u8>(priority[j] | (priority[j + 16] << 4));

    Selection sel;
    std::array<u8, CELL_SPURS_MAX_WORKLOAD2> contention{};
    alignas(16) u8 final_line[PS_RES_LINE_SIZE]; // the line as stored, or as read when nothing changed
    reservation_op(base, spurs, [&](u8* line) {
        sel = Selection{};
        for (u32 i = 0; i < CELL_SPURS_MAX_WORKLOAD2; i++) {
            const u8 c = static_cast<u8>(ps_read8(line, wkl_off::currentContention + (i & 15)) - loc_contention[i & 15]);
            contention[i] = i < CELL_SPURS_MAX_WORKLOAD ? c & 0x0F : c >> 4;
        }
        bool changed = false;
        // The system service's message bit for this SPU: acknowledged (its work is read from the guest state above).
        const u8 message = ps_read8(line, wkl_off::sysSrvMessage);
        if (!poll && (message & (1u << spu_num))) {
            ps_write8(line, wkl_off::sysSrvMessage, static_cast<u8>(message & ~(1u << spu_num)));
            changed = true;
        }
        const u32 flag = ps_read32(line, spurs_off::wklFlag);
        const u8 flag_receiver = ps_read8(line, spurs_off::wklFlagReceiver);
        u8 max_weight = 0;
        for (u32 i = 0; i < CELL_SPURS_MAX_WORKLOAD2; i++) {
            const u32 j = i & 15;
            const bool is_runnable = runnable & (0x80000000u >> i);
            const bool wkl_signal = ps_read16(line, wkl_signal_of(i)) & wkl_signal_bit(i);
            const bool wkl_flag = flag == 0 && flag_receiver == i;
            const u8 ready = ps_read8(line, wkl_ready(i));
            const u8 max_byte = ps_read8(line, wkl_off::maxContention + j);
            const u8 max_contention = i < CELL_SPURS_MAX_WORKLOAD ? max_byte & 0x0F : max_byte >> 4;
            if (!is_runnable || priority[i] == 0 || max_contention <= contention[i]) continue;
            if (!(wkl_flag || wkl_signal || ready > contention[i])) continue;
            const u8 weight = static_cast<u8>((priority[i] << 4) | (i == current_wid ? 0x04 : 0));
            if (weight > max_weight) {
                max_weight = weight;
                sel.wid = i;
                sel.serial = max_contention == 1;
                sel.poll_status = (ready > contention[i] ? CELL_SPURS_MODULE_POLL_STATUS_READYCOUNT : 0) |
                                  (wkl_signal ? CELL_SPURS_MODULE_POLL_STATUS_SIGNAL : 0) |
                                  (wkl_flag ? CELL_SPURS_MODULE_POLL_STATUS_FLAG : 0);
            }
        }
        if (poll) {
            // As kernel 1's below: a poll that keeps the current workload consumes its signal and the workload flag.
            if (sel.wid != current_wid || sel.wid >= CELL_SPURS_MAX_WORKLOAD2) return changed;
            const u16 signal = ps_read16(line, wkl_signal_of(sel.wid));
            const bool take_flag = sel.wid == flag_receiver && flag != 0xFFFFFFFF;
            if (!(signal & wkl_signal_bit(sel.wid)) && !take_flag) return changed;
            ps_write16(line, wkl_signal_of(sel.wid), static_cast<u16>(signal & ~wkl_signal_bit(sel.wid)));
            if (take_flag) ps_write32(line, spurs_off::wklFlag, 0xFFFFFFFF);
            return true;
        }
        if (sel.wid < CELL_SPURS_MAX_WORKLOAD2) {
            // Changed only where the bytes change (D-972): a worker picking its own workload again (GTA V's job chains,
            // over and over) stores nothing, so the line's version stays and its waiters sleep on.
            const u16 signal = ps_read16(line, wkl_signal_of(sel.wid));
            if (signal & wkl_signal_bit(sel.wid)) {
                ps_write16(line, wkl_signal_of(sel.wid), static_cast<u16>(signal & ~wkl_signal_bit(sel.wid)));
                changed = true;
            }
            if (sel.wid == flag_receiver && flag != 0xFFFFFFFF) {
                ps_write32(line, spurs_off::wklFlag, 0xFFFFFFFF);
                changed = true;
            }
            contention[sel.wid]++;
        }
        for (u32 j = 0; j < CELL_SPURS_MAX_WORKLOAD; j++) {
            const u8 v = static_cast<u8>((contention[j] & 0x0F) | (contention[j + 16] << 4));
            changed |= ps_read8(line, wkl_off::currentContention + j) != v;
            ps_write8(line, wkl_off::currentContention + j, v);
        }
        std::memcpy(final_line, line, PS_RES_LINE_SIZE);
        return changed; // nothing stored while nothing changes (see select1)
    });
    if (poll) return sel;

    loc_contention.fill(0);
    if (sel.wid < CELL_SPURS_MAX_WORKLOAD2) loc_contention[sel.wid & 15] = sel.wid < CELL_SPURS_MAX_WORKLOAD ? 0x01 : 0x10;
    current_wid = sel.wid;
    update_kernel_context(packed, unique, sel.wid, final_line);
    return sel;
}

Worker::Selection Worker::select1(bool poll)
{
    u8* base = sched.base;
    const u32 spurs = sched.spurs;
    // What the system service keeps in each SPU's kernel context, read from the guest state: which workloads are
    // runnable, their priority on this SPU and unique IDs. The game's priorities run from 1 (highest) to 15, 0 meaning
    // "not on this SPU"; the kernel weighs 16 - priority (0 stays 0), as libsre's system service converts them (fw 4.92;
    // RPCS3 spursSysServiceUpdateWorkload).
    std::array<u8, CELL_SPURS_MAX_WORKLOAD> priority{}, unique{};
    u16 runnable = 0;
    // Read atomically, each state before the priority it makes valid, as in select2 (D-587).
    const u32 enabled = atomic_load_be32(base, spurs + wkl_off::enabled);
    const u32 unsupported = sched.unsupported.load(std::memory_order_relaxed);
    for (u32 i = 0; i < CELL_SPURS_MAX_WORKLOAD; i++) {
        const bool listed = (enabled & (0x80000000u >> i)) && !(unsupported & (1u << i));
        if (listed && atomic_load_u8(base, spurs + wkl_off::state1 + i) == SPURS_WKL_STATE_RUNNABLE) {
            runnable |= static_cast<u16>(0x8000u >> i);
        }
        const u8 p = atomic_load_u8(base, wkl_info(spurs, i) + wkl_off::info_priority + spu_num);
        priority[i] = p ? static_cast<u8>(0x10 - p) : 0;
        unique[i] = atomic_load_u8(base, wkl_info(spurs, i) + wkl_off::info_unique);
    }

    Selection sel;
    std::array<u8, CELL_SPURS_MAX_WORKLOAD> contention{};
    alignas(16) u8 final_line[PS_RES_LINE_SIZE]; // the line as stored, or as read when nothing changed
    reservation_op(base, spurs, [&](u8* line) {
        sel = Selection{};
        for (u32 i = 0; i < CELL_SPURS_MAX_WORKLOAD; i++) {
            contention[i] = static_cast<u8>(ps_read8(line, wkl_off::currentContention + i) - loc_contention[i]);
        }
        bool changed = false;
        // The system service's message bit for this SPU: acknowledged (its work is read from the guest state above).
        const u8 message = ps_read8(line, wkl_off::sysSrvMessage);
        if (!poll && (message & (1u << spu_num))) {
            ps_write8(line, wkl_off::sysSrvMessage, static_cast<u8>(message & ~(1u << spu_num)));
            changed = true;
        }
        const u16 signal = ps_read16(line, wkl_off::signal1);
        const u32 flag = ps_read32(line, spurs_off::wklFlag);
        const u8 flag_receiver = ps_read8(line, spurs_off::wklFlagReceiver);
        u16 max_weight = 0;
        for (u32 i = 0; i < CELL_SPURS_MAX_WORKLOAD; i++) {
            const bool is_runnable = runnable & (0x8000u >> i);
            const bool wkl_signal = signal & (0x8000u >> i);
            const bool wkl_flag = flag == 0 && flag_receiver == i;
            const u8 ready = std::min<u8>(ps_read8(line, wkl_off::readyCount1 + i), CELL_SPURS_MAX_SPU);
            const u8 idle = std::min<u8>(ps_read8(line, wkl_off::idleSpuCountOrReadyCount2 + i), CELL_SPURS_MAX_SPU);
            const u8 request = static_cast<u8>(ready + idle);
            const u8 max_contention = ps_read8(line, wkl_off::maxContention + i) & 0x0F;
            if (!is_runnable || priority[i] == 0 || max_contention <= contention[i]) continue;
            if (!(wkl_flag || wkl_signal || (ready != 0 && request > contention[i]))) continue;
            // Weight, highest first: work to do; priority on this SPU; already running here; below its minimum
            // contention; fewer SPUs on it; same policy module as the current one; lower ID (ties keep the first).
            u16 weight = (wkl_flag || wkl_signal || ready > contention[i]) ? 0x8000 : 0;
            weight |= static_cast<u16>((priority[i] & 0x7F) << 8);
            weight |= i == current_wid ? 0x80 : 0;
            weight |= (contention[i] > 0 && ps_read8(line, wkl_off::minContention + i) > contention[i]) ? 0x40 : 0;
            weight |= static_cast<u16>(((CELL_SPURS_MAX_SPU - contention[i]) & 0x0F) << 2);
            weight |= current_wid < CELL_SPURS_MAX_WORKLOAD && unique[i] == unique[current_wid] ? 0x02 : 0;
            weight |= 0x01;
            if (weight > max_weight) {
                max_weight = weight;
                sel.wid = i;
                sel.serial = max_contention == 1;
                sel.poll_status = (ready > contention[i] ? CELL_SPURS_MODULE_POLL_STATUS_READYCOUNT : 0) |
                                  (wkl_signal ? CELL_SPURS_MODULE_POLL_STATUS_SIGNAL : 0) |
                                  (wkl_flag ? CELL_SPURS_MODULE_POLL_STATUS_FLAG : 0);
            }
        }
        if (poll) {
            // A poll that keeps the current workload consumes its signal and the workload flag, as a dispatch does
            // (libsre kernel 1 select: !isPoll || selected == current; RPCS3 spursKernel1SelectWorkload). A chain or
            // taskset signalled again while it runs is then picked up by this poll, not left signalled forever.
            if (sel.wid != current_wid || sel.wid >= CELL_SPURS_MAX_WORKLOAD) return changed;
            const u16 bit = static_cast<u16>(0x8000u >> sel.wid);
            const bool take_flag = sel.wid == flag_receiver && flag != 0xFFFFFFFF;
            if (!(signal & bit) && !take_flag) return changed;
            ps_write16(line, wkl_off::signal1, static_cast<u16>(signal & ~bit));
            if (take_flag) ps_write32(line, spurs_off::wklFlag, 0xFFFFFFFF);
            return true;
        }
        if (sel.wid < CELL_SPURS_MAX_WORKLOAD) {
            // Changed only where the bytes change (D-972): a worker picking its own workload again stores nothing.
            const u16 bit = static_cast<u16>(0x8000u >> sel.wid);
            if (signal & bit) {
                ps_write16(line, wkl_off::signal1, static_cast<u16>(signal & ~bit));
                changed = true;
            }
            if (sel.wid == flag_receiver && flag != 0xFFFFFFFF) {
                ps_write32(line, spurs_off::wklFlag, 0xFFFFFFFF);
                changed = true;
            }
            contention[sel.wid]++;
        }
        for (u32 i = 0; i < CELL_SPURS_MAX_WORKLOAD; i++) {
            changed |= ps_read8(line, wkl_off::currentContention + i) != contention[i];
            ps_write8(line, wkl_off::currentContention + i, contention[i]);
        }
        std::memcpy(final_line, line, PS_RES_LINE_SIZE);
        // An SPU that stays idle and changes nothing stores nothing: every store bumps the line's version, which would
        // end its own wait for a change at once (regression: idle workers spun at full speed instead of sleeping).
        return changed;
    });
    if (poll) return sel;

    loc_contention.fill(0);
    if (sel.wid < CELL_SPURS_MAX_WORKLOAD) loc_contention[sel.wid] = 1;
    current_wid = sel.wid;
    update_kernel_context(priority, unique, sel.wid, final_line);
    return sel;
}

bool Worker::run_workload(const Selection& sel)
{
    Workload wkl;
    bool known = false;
    {
        std::lock_guard lock(sched.host_mutex);
        if (const auto it = sched.inst.workloads.find(sel.wid); it != sched.inst.workloads.end()) {
            wkl = it->second;
            known = true;
        }
    }
    const u32 info = wkl_info(sched.spurs, sel.wid);
    if (known && wkl.kind == Workload::Kind::Taskset) {
        ps_write64(ls, ls::k_wkl_addr, ps_read64(sched.base, info + wkl_off::info_addr));
        ps_write32(ls, ls::k_wkl_unique, ps_read8(sched.base, info + wkl_off::info_unique));
        run_taskset(*this, sel.wid, wkl.arg, sel.poll_status);
        return false;
    }
    if (known && wkl.kind == Workload::Kind::Custom) {
        run_custom(*this, sel.wid, sel.poll_status);
        return true;
    }
    if (known && wkl.kind == Workload::Kind::JobChain) {
        // The job chain module's image replaces whatever policy module was in local store.
        ps_write64(ls, ls::k_wkl_addr, ps_read64(sched.base, info + wkl_off::info_addr));
        ps_write32(ls, ls::k_wkl_unique, ps_read8(sched.base, info + wkl_off::info_unique));
        run_jobchain(*this, sel.wid, wkl.arg);
        return false;
    }
    // A workload the HLE API did not register: logged once, then left out of the selection (CLAUDE.md rule 5: loud,
    // not silent).
    const u32 bit = 1u << sel.wid;
    if (!(sched.unsupported.fetch_or(bit) & bit)) {
        const u64 pm = ps_read64(sched.base, info + wkl_off::info_addr);
        PS_LOG_ERROR(kSchedChan, "SPURS {:#x}: workload {} (policy module {:#x}, argument {:#x}) has work, but the native "
            "scheduler does not know it; it does not run", sched.spurs, sel.wid, pm,
            ps_read64(sched.base, info + wkl_off::info_arg));
    }
    return false;
}

// ------------------------------------------------------------------------------------------------- scheduler

Scheduler::Scheduler(std::mutex& mutex, lv2::Kernel& k, u32 spurs_addr, Instance& instance)
    : host_mutex(mutex), kernel(k), base(k.base()), spurs(spurs_addr), inst(instance), second(instance.second)
{
}

Scheduler::~Scheduler() { stop(); }

void Scheduler::start()
{
#if defined(__APPLE__)
    if (const char* v = std::getenv("PSPOCKET_SPU_QOS_SERIAL"); v && *v) {
        qos_serial = qos_named(v);
        PS_LOG_INFO(kSchedChan, "SPURS {:#x}: workloads of one SPU at a time run at QoS {}", spurs, qos_name(qos_serial));
    }
#endif
    // Every SPU a host thread: a workload runs on the SPUs its priorities name (GTA V's EDGE on three of five, its
    // GraphicsMain on one), so fewer threads would leave some never run.
    if (const char* v = std::getenv("PSPOCKET_SPU_AB"); v && std::atoi(v) > 0) {
        ab_period_ns = static_cast<u64>(std::atoi(v)) * 1'000'000'000ull;
#if defined(__APPLE__)
        qos_a = worker_qos(inst.nspus);
        qos_b = std::getenv("PSPOCKET_SPU_QOS_B") ? qos_named(std::getenv("PSPOCKET_SPU_QOS_B")) : qos_a;
        PS_LOG_INFO(kSchedChan, "SPURS {:#x}: every {} s QoS {} (A) and {} (B) in turn", spurs, std::atoi(v),
            qos_name(qos_a), qos_name(qos_b));
#endif
    }
    for (u32 i = 0; i < inst.nspus; i++) {
        workers_.push_back(std::make_unique<Worker>(*this, i));
        if (!workers_.back()->start()) {
            workers_.pop_back();
            break;
        }
    }
    PS_LOG_INFO(kSchedChan, "SPURS {:#x}: native scheduler with {} SPU workers", spurs, workers_.size());
}

void Scheduler::stop()
{
    if (stopping.exchange(true)) return;
    for (auto& w : workers_) w->request_stop();
    ps_res_notify_write(spurs, PS_RES_LINE_SIZE);
    for (auto& w : workers_) w->join();
    workers_.clear();
}

void add_ready_count(Worker& w, u32 wid, s32 n)
{
    // libsre's policy module updates the ready count after every request, even by 0 (the PUTLLC also wakes idle SPUs).
    // A decrement larger than the count is carried until other SPUs have added enough.
    s32 carry = n;
    const auto start = std::chrono::steady_clock::now();
    bool warned = false;
    for (;;) {
        // The update runs again whenever another writer got in first (reservation_op), so it computes only from its
        // input and the line: `carry` changes after the update is stored, never inside it.
        s32 left = 0;
        reservation_op(w.sched.base, w.sched.spurs, [&](u8* line) {
            const s32 now = ps_read8(line, wkl_ready(wid)) + carry;
            ps_write8(line, wkl_ready(wid), static_cast<u8>(std::clamp(now, 0, 255)));
            left = now < 0 ? now : 0;
            return true;
        });
        carry = left;
        if (carry >= 0 || w.sched.stopping.load(std::memory_order_relaxed)) return;
        if (!warned && std::chrono::steady_clock::now() - start > std::chrono::seconds(1)) {
            PS_LOG_WARN(kSchedChan, "SPURS {:#x} SPU {}: workload {}'s ready count is {} short of a decrement; waiting",
                w.sched.spurs, w.spu_num, wid, -carry);
            warned = true;
        }
        std::this_thread::yield();
    }
}

// ------------------------------------------------------------------------------------------------- API

void start_scheduler(SpursHost& host, lv2::Kernel& kernel, u32 spurs, Instance& inst)
{
    inst.scheduler = std::make_shared<Scheduler>(host.mutex, kernel, spurs, inst);
    inst.scheduler->start();
}

void stop_scheduler_of(Scheduler& scheduler) { scheduler.stop(); }

void wake_workers(u32 spurs) { ps_res_notify_write(spurs, PS_RES_LINE_SIZE); }

u32 workers_inside(const Scheduler& scheduler, u32 wid)
{
    return wid < CELL_SPURS_MAX_WORKLOAD2 ? scheduler.inside[wid].load(std::memory_order_seq_cst) : 0;
}

// RPCS3 cellSpurs.cpp _cellSpursSendSignal's spurs_taskset_signal_op; libsre's PPU side does the same on the task's
// 32-bit word.
int signal_task(u8* base, u32 taskset, u32 task_id)
{
    const u32 word = 4 * (task_id / 32), mask = 0x80000000u >> (task_id % 32);
    int signal = -1;
    reservation_op(base, taskset, [&](u8* line) {
        const u32 running = ps_read32(line, 0x00 + word), ready = ps_read32(line, 0x10 + word),
                  pready = ps_read32(line, 0x20 + word), enabled = ps_read32(line, 0x30 + word),
                  signalled = ps_read32(line, 0x40 + word), waiting = ps_read32(line, 0x50 + word);
        if ((running & waiting) || (ready & pready) || ((signalled | waiting | pready | running | ready) & ~enabled) ||
            !(enabled & mask)) {
            signal = -1;
            return false;
        }
        signal = (~signalled & waiting & mask) ? 1 : 0;
        ps_write32(line, 0x40 + word, signalled | mask);
        return true;
    });
    return signal;
}

// _cellSpursSendSignal (RPCS3 cellSpurs.cpp; libsre's PPU side does the same on the 32-bit word): set the task's
// signalled bit; if it was waiting and not yet signalled, signal its workload so a worker picks the taskset up.
s32 send_signal(u8* base, u32 taskset, u32 task_id)
{
    if (!taskset) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (taskset % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (task_id >= 128 || ps_read32(base, taskset + ts_off::wid) >= CELL_SPURS_MAX_WORKLOAD2) {
        return error(CELL_SPURS_TASK_ERROR_INVAL);
    }
    const int signal = signal_task(base, taskset, task_id);
    if (signal < 0) return error(CELL_SPURS_TASK_ERROR_SRCH);
    if (signal == 1) {
        const u32 spurs = static_cast<u32>(ps_read64(base, taskset + ts_off::spurs));
        const u32 wid = ps_read32(base, taskset + ts_off::wid);
        reservation_op(base, spurs, [wid](u8* line) {
            ps_write16(line, wkl_signal_of(wid), static_cast<u16>(ps_read16(line, wkl_signal_of(wid)) | wkl_signal_bit(wid)));
            return true;
        });
        if (ps_read32(base, spurs + wkl_off::exception)) return error(CELL_SPURS_TASK_ERROR_STAT);
    }
    return lv2::CELL_OK;
}

} // namespace pspocket::hle::spurs
