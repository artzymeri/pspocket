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
// Deviations from the SPURS kernel: the system service workload never runs (its jobs, activating workloads and
// processing their state changes, are read from the guest state directly, and its message bit is just acknowledged);
// pending contention (switch requests from polls) is not tracked; SPURS2 (32 workloads) is not supported yet.

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
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/spu/fp.h"
#include "pspocket/spu/runtime.h"
#include "spurs_native.hpp"

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace pspocket::hle::spurs {
namespace {

constexpr std::string_view kSchedChan = "spurs";
constexpr u64 kIdleWaitNs = 100'000'000; // re-check for a stop request this often while idle
constexpr usize kWorkerStack = 8u << 20;  // as SPU threads (D-015)
constexpr u32 CELL_SPURS_MODULE_POLL_STATUS_READYCOUNT = 1, CELL_SPURS_MODULE_POLL_STATUS_SIGNAL = 2,
              CELL_SPURS_MODULE_POLL_STATUS_FLAG = 4;
constexpr u64 SYS_SPU_THREAD_EVENT_USER_KEY = 0xFFFFFFFF53505501ull;

// Worker contexts handle their own stops: every stop returns to the worker (the syscall trap, and anything else,
// which the worker reports).
int worker_stop(SPUContext*, uint32_t) { return 0; }

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

constexpr ps_spu_hooks kWorkerHooks{worker_stop, worker_intr_mbox};

u32 info_addr(u32 spurs, u32 wid) { return spurs + wkl_off::info1 + wid * wkl_off::info_stride; }

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
    ps_write32(ls, ls::k_exit_to_kernel, kKernelExit);
    ps_write32(ls, ls::k_select, kKernelSelect);
    // The kernel's entry points are `stop`s the worker takes (no Sony kernel code runs).
    ps_write32(ls, kKernelSelect, kKernelSelectStop);
    ps_write32(ls, kKernelReturn, kKernelExitStop);
    ps_write32(ls, kKernelExit, kKernelExitStop);
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
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INITIATED, 0);
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
    while (!sched.stopping.load(std::memory_order_acquire)) {
        // The version is read before selecting, so a change made while selecting ends the wait at once.
        const u64 version = ps_res_version(sched.spurs);
        const Selection sel = select(false);
        if (sel.wid >= CELL_SPURS_MAX_WORKLOAD) {
            ps_res_wait_change(sched.spurs, version, kIdleWaitNs);
            continue;
        }
        run_workload(sel);
    }
    release();
    ps_spu_fpenv_leave(fp_env);
}

// Gives this SPU's contention back (the worker stops).
void Worker::release()
{
    if (current_wid >= CELL_SPURS_MAX_WORKLOAD) return;
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

Worker::Selection Worker::select(bool poll)
{
    u8* base = sched.base;
    const u32 spurs = sched.spurs;
    // What the system service keeps in each SPU's kernel context, read from the guest state: which workloads are
    // runnable, their priority on this SPU and unique IDs. The game's priorities run from 1 (highest) to 15, 0 meaning
    // "not on this SPU"; the kernel weighs 16 - priority (0 stays 0), as libsre's system service converts them (fw 4.92;
    // RPCS3 spursSysServiceUpdateWorkload).
    std::array<u8, CELL_SPURS_MAX_WORKLOAD> priority{}, unique{};
    u16 runnable = 0;
    const u32 enabled = ps_read32(base, spurs + wkl_off::enabled);
    const u32 unsupported = sched.unsupported.load(std::memory_order_relaxed);
    for (u32 i = 0; i < CELL_SPURS_MAX_WORKLOAD; i++) {
        const u8 p = ps_read8(base, info_addr(spurs, i) + wkl_off::info_priority + spu_num);
        priority[i] = p ? static_cast<u8>(0x10 - p) : 0;
        unique[i] = ps_read8(base, info_addr(spurs, i) + wkl_off::info_unique);
        if ((enabled & (0x80000000u >> i)) && ps_read8(base, spurs + wkl_off::state1 + i) == SPURS_WKL_STATE_RUNNABLE &&
            !(unsupported & (1u << i))) {
            runnable |= static_cast<u16>(0x8000u >> i);
        }
    }

    Selection sel;
    std::array<u8, CELL_SPURS_MAX_WORKLOAD> contention{};
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
            ps_write16(line, wkl_off::signal1, static_cast<u16>(signal & ~(0x8000u >> sel.wid)));
            if (sel.wid == flag_receiver) ps_write32(line, spurs_off::wklFlag, 0xFFFFFFFF);
            contention[sel.wid]++;
            changed = true;
        }
        for (u32 i = 0; i < CELL_SPURS_MAX_WORKLOAD; i++) {
            changed |= ps_read8(line, wkl_off::currentContention + i) != contention[i];
            ps_write8(line, wkl_off::currentContention + i, contention[i]);
        }
        // An SPU that stays idle and changes nothing stores nothing: every store bumps the line's version, which would
        // end its own wait for a change at once (regression: idle workers spun at full speed instead of sleeping).
        return changed;
    });
    if (poll) return sel;

    loc_contention.fill(0);
    if (sel.wid < CELL_SPURS_MAX_WORKLOAD) loc_contention[sel.wid] = 1;
    current_wid = sel.wid;
    // The kernel context in local store: the CellSpurs line 0 copy (task code reads flags1 at 0x174 from it), the
    // priorities, the current workload.
    alignas(16) u8 line0[PS_RES_LINE_SIZE];
    ps_res_acquire(base, spurs, line0);
    std::memcpy(ls + ls::kernel, line0, PS_RES_LINE_SIZE);
    std::memcpy(ls + ls::k_priority, priority.data(), priority.size());
    std::memcpy(ls + ls::k_unique_ids, unique.data(), unique.size());
    // wklCurrentAddr (0x1D0) and the unique ID are the dispatch's: a custom policy module is reloaded only when the
    // address changes.
    ps_write32(ls, ls::k_wkl_id, sel.wid);
    return sel;
}

void Worker::run_workload(const Selection& sel)
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
    if (known && wkl.kind == Workload::Kind::Taskset) {
        ps_write64(ls, ls::k_wkl_addr, ps_read64(sched.base, info_addr(sched.spurs, sel.wid) + wkl_off::info_addr));
        ps_write32(ls, ls::k_wkl_unique, ps_read8(sched.base, info_addr(sched.spurs, sel.wid) + wkl_off::info_unique));
        run_taskset(*this, sel.wid, wkl.arg, sel.poll_status);
        return;
    }
    if (known && wkl.kind == Workload::Kind::Custom) {
        run_custom(*this, sel.wid, sel.poll_status);
        return;
    }
    if (known && wkl.kind == Workload::Kind::JobChain) {
        // The job chain module's image replaces whatever policy module was in local store.
        ps_write64(ls, ls::k_wkl_addr, ps_read64(sched.base, info_addr(sched.spurs, sel.wid) + wkl_off::info_addr));
        ps_write32(ls, ls::k_wkl_unique, ps_read8(sched.base, info_addr(sched.spurs, sel.wid) + wkl_off::info_unique));
        run_jobchain(*this, sel.wid, wkl.arg);
        return;
    }
    // A workload the HLE API did not register: logged once, then left out of the selection (CLAUDE.md rule 5: loud,
    // not silent).
    const u32 bit = 1u << sel.wid;
    if (!(sched.unsupported.fetch_or(bit) & bit)) {
        const u64 pm = ps_read64(sched.base, info_addr(sched.spurs, sel.wid) + wkl_off::info_addr);
        PS_LOG_ERROR(kSchedChan, "SPURS {:#x}: workload {} (policy module {:#x}, argument {:#x}) has work, but the native "
            "scheduler does not know it; it does not run", sched.spurs, sel.wid, pm,
            ps_read64(sched.base, info_addr(sched.spurs, sel.wid) + wkl_off::info_arg));
    }
}

// ------------------------------------------------------------------------------------------------- scheduler

Scheduler::Scheduler(std::mutex& mutex, lv2::Kernel& k, u32 spurs_addr, Instance& instance)
    : host_mutex(mutex), kernel(k), base(k.base()), spurs(spurs_addr), inst(instance)
{
}

Scheduler::~Scheduler() { stop(); }

void Scheduler::start()
{
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
            const s32 now = ps_read8(line, wkl_off::readyCount1 + wid) + carry;
            ps_write8(line, wkl_off::readyCount1 + wid, static_cast<u8>(std::clamp(now, 0, 255)));
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
    if (inst.second) {
        PS_LOG_ERROR(kSchedChan, "SPURS {:#x}: SPURS2 (32 workloads) has no native scheduler yet; nothing runs", spurs);
        return;
    }
    inst.scheduler = std::make_shared<Scheduler>(host.mutex, kernel, spurs, inst);
    inst.scheduler->start();
}

void stop_scheduler_of(Scheduler& scheduler) { scheduler.stop(); }

void wake_workers(u32 spurs) { ps_res_notify_write(spurs, PS_RES_LINE_SIZE); }

// _cellSpursSendSignal (RPCS3 cellSpurs.cpp; libsre's PPU side does the same on the 32-bit word): set the task's
// signalled bit; if it was waiting and not yet signalled, signal its workload so a worker picks the taskset up.
s32 send_signal(u8* base, u32 taskset, u32 task_id)
{
    if (!taskset) return error(CELL_SPURS_TASK_ERROR_NULL_POINTER);
    if (taskset % 128) return error(CELL_SPURS_TASK_ERROR_ALIGN);
    if (task_id >= 128 || ps_read32(base, taskset + ts_off::wid) >= CELL_SPURS_MAX_WORKLOAD2) {
        return error(CELL_SPURS_TASK_ERROR_INVAL);
    }
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
    if (signal < 0) return error(CELL_SPURS_TASK_ERROR_SRCH);
    if (signal == 1) {
        const u32 spurs = static_cast<u32>(ps_read64(base, taskset + ts_off::spurs));
        const u32 wid = ps_read32(base, taskset + ts_off::wid);
        reservation_op(base, spurs, [wid](u8* line) {
            ps_write16(line, wkl_off::signal1, static_cast<u16>(ps_read16(line, wkl_off::signal1) | (0x8000u >> wid)));
            return true;
        });
        if (ps_read32(base, spurs + wkl_off::exception)) return error(CELL_SPURS_TASK_ERROR_STAT);
    }
    return lv2::CELL_OK;
}

} // namespace pspocket::hle::spurs
