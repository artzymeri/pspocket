#pragma once

// The native SPURS scheduler's workers (D-037 stage 3b): shared by spurs_scheduler.cpp (workers, workload selection,
// SPU-side events) and the policies: spurs_taskset.cpp (tasksets), spurs_jobchain.cpp (job chains) and
// spurs_custom.cpp (a game's own policy module). Internal to runtime/hle.
//
// A worker stands for one SPU of a SPURS instance: a host thread with an SPU context and 256 KB of local store. Its
// local store below 0x3000 holds what Sony's SPURS kernel and taskset policy module keep there, as far as task code
// reads it (layout from libsre, docs/decisions.md D-050): the kernel context at 0x100 and the taskset context at 0x2700.
// No Sony code runs: the policy module's syscall entry (0xA70) holds a `stop` the worker catches.

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/spu/context.h"
#include "spurs.hpp"

namespace pspocket::hle::spurs {

// LS layout below 0x3000 (libsre; the kernel context and taskset context offsets match RPCS3's SpursKernelContext and
// SpursTasksetContext).
namespace ls {
constexpr u32 kernel = 0x100;           // kernel context; 0x100..0x17F: copy of CellSpurs line 0
constexpr u32 k_priority = 0x1A0;       // per-workload priority on this SPU
constexpr u32 k_spurs = 0x1C0;          // u64
constexpr u32 k_spu_num = 0x1C8;
constexpr u32 k_dma_tag = 0x1CC;        // 31
constexpr u32 k_wkl_addr = 0x1D0;       // u64: the current workload's policy module address
constexpr u32 k_wkl_unique = 0x1D8;
constexpr u32 k_wkl_id = 0x1DC;         // read by tasks (event flag waits)
constexpr u32 k_exit_to_kernel = 0x1E0; // 0x808 (kernel 1)
constexpr u32 k_select = 0x1E4;         // 0x290
constexpr u32 k_module_id = 0x1E8;      // u16 'TK' while the taskset policy runs; task code checks it
constexpr u32 k_unique_ids = 0x220;     // wklUniqueId[16]
constexpr u32 pm_entry = 0xA00;
constexpr u32 pm_syscall = 0xA70;       // the syscall entry tasks call
constexpr u32 ts = 0x2700;              // taskset context: copy of the taskset's first line
constexpr u32 ts_task_info = 0x2780;    // the current task's TaskInfo (48 bytes)
constexpr u32 ts_ti_context = 0x2798;   // u64 context | allocated blocks (read by tasks)
constexpr u32 ts_ti_pattern = 0x27A0;   // LS pattern (read by tasks)
constexpr u32 ts_taskset = 0x27B8;      // u64
constexpr u32 ts_kernel = 0x27C0;
constexpr u32 ts_syscall = 0x27C4;      // 0xA70 (read by tasks for every syscall)
constexpr u32 ts_spu_num = 0x27CC;
constexpr u32 ts_dma_tag = 0x27D0;
constexpr u32 ts_task_id = 0x27D4;      // read by tasks
constexpr u32 ts_module_id = 0x2840;    // "SPURSTASK MODULE"
constexpr u32 pm_stack = 0x2C30;        // $1 at task start
constexpr u32 ctx_area = 0x2C80;        // the execution context: 0x380 bytes, saved and restored as a block
constexpr u32 ctx_lr = 0x2C80, ctx_sp = 0x2C90, ctx_r80 = 0x2CA0, ctx_fpscr = 0x2FA0, ctx_tag_mask = 0x2FB0,
              ctx_event_mask = 0x2FB4, ctx_mgmt = 0x2FB8, ctx_guid = 0x2FBC, ctx_exit_cb = 0x2FC0,
              ctx_exit_arg = 0x2FC8, ctx_exit_code = 0x2FD0, ctx_flags = 0x2FD4, ctx_wait_object = 0x2FD8;
constexpr u32 ctx_area_size = 0x380;
constexpr u32 task_top = 0x3000;
constexpr u32 exit_callback = 0x10000;       // where an exiting task's callback is loaded (RPCS3)
constexpr u32 exit_callback_return = 0xA80;  // its return: a `stop` in the taskset policy module's unused space
// Job chains (libsre's job chain policy module, D-055): a job returns to 0x2570 (after the module's call), its context
// (CellSpursJobContext2) is at 0x4940, and binaries and buffers are 1 KB pages from 0x4C00 up to the 4 descriptor
// buffers, which end at 0x3F800 (the module's stack is above).
constexpr u32 jc_return = 0x2570;
constexpr u32 jc_context = 0x4940;
constexpr u32 jc_heap = 0x4C00;
constexpr u32 jc_descriptors_end = 0x3F800;
constexpr u32 size = 0x40000;
} // namespace ls

// Stop codes the worker plants in local store (any codes work: worker contexts have their own stop hook): the taskset
// policy's syscall entry, and the SPURS kernel's entry points a game's own policy module may reach (spurs_custom.cpp):
// select workload (0x290, [0x1E4]), the return from a module (kernel 1: 0x804) and the exit to the kernel (kernel 1:
// 0x808, [0x1E0]). SPURS2's kernel 2 (libsre's second SPU kernel, entry 0x848) calls a module from 0x834, so a module
// returns to 0x838, which is also its exit ([0x1E0]; RPCS3 CELL_SPURS_KERNEL2_EXIT_ADDR).
constexpr u32 kSyscallStop = ls::pm_syscall;
// The return of a task's exit callback (spurs_taskset.cpp).
constexpr u32 kExitCallbackStop = ls::exit_callback_return;
// CellSpursTaskset2's per-task exit callbacks (16 bytes each: descriptor, argument; RPCS3 task_exit_code[128]).
constexpr u32 kTaskExitCallbacks = 0x1980;
// The return address of a job (job chains).
constexpr u32 kJobReturnStop = ls::jc_return;
constexpr u32 kKernelSelect = 0x290, kKernelReturn = 0x804, kKernelExit = 0x808, kKernel2Exit = 0x838;
constexpr u32 kKernelSelectStop = kKernelSelect, kKernelExitStop = kKernelExit;
// "Idle": the SPURS kernel's ID for the system service workload, which a native worker never runs.
constexpr u32 kIdleWid = 0x20;

class Scheduler;

class Worker {
public:
    Worker(Scheduler& scheduler, u32 spu_num);
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    bool start();
    void request_stop();
    void join();

    Scheduler& sched;
    const u32 spu_num;
    u8* ls = nullptr; // 256 KB, 128-byte aligned
    SPUContext ctx{};

    // Kernel bookkeeping (RPCS3 SpursKernelContext): this SPU's share of each workload's contention (SPURS2: packed
    // as CellSpurs::wklCurrentContention, wid & 15's byte, 0x01 or 0x10), the current workload.
    std::array<u8, 16> loc_contention{};
    u32 current_wid = kIdleWid;
    // A game's own policy module is reloaded whenever another policy ran in between: logged once per worker.
    bool custom_load_logged = false;
    const ps_spu_program* custom_program = nullptr; // the lifted program of the policy module in LS (D-076)
    // With none, the module's key and the registrations it was looked up after (D-234): looked up again when programs
    // are registered during the session, unless a DMA over its code dropped the one it had.
    u64 custom_hash = 0, custom_additions = 0;
    u32 custom_size = 0;
    bool custom_dropped = false;

    struct Selection {
        u32 wid = kIdleWid;
        u32 poll_status = 0; // CELL_SPURS_MODULE_POLL_STATUS_*: 1 ready count, 2 signal, 4 workload flag
    };
    // The SPURS kernel's workload selection (RPCS3 spursKernel1SelectWorkload; for SPURS2 spursKernel2SelectWorkload)
    // on the guest CellSpurs. `poll` asks whether another workload should take this SPU without changing anything
    // (cellSpursModulePollStatus).
    Selection select(bool poll);
    // True when the policy running on this worker should give the SPU back to the kernel.
    bool poll_switch() { return select(true).wid != current_wid; }

    // Where this worker's time went (D-103): running workloads, running a game's own policy module that changed nothing
    // in main memory (a poll that found no work), and waiting for work.
    u64 ns_work = 0, ns_idle_polls = 0, ns_wait = 0, idle_polls = 0;
    u32 idle_streak = 0; // empty polls in a row
    u64 backoff_ns = 0;  // the wait after the next one

private:
    static void* entry(void* arg);
    void body();
    bool run_workload(const Selection& sel); // true: a game's own policy module ran
    void release();
    Selection select1(bool poll); // kernel 1: SPURS1, workloads 0..15
    Selection select2(bool poll); // kernel 2: SPURS2, workloads 0..31
    // After a selection: the kernel context in local store (CellSpurs line 0's copy, priorities, unique IDs, the
    // current workload ID).
    void update_kernel_context(const std::array<u8, 16>& priority, const std::array<u8, 16>& unique, u32 wid);

    void* thread_ = nullptr; // pthread_t
    bool joinable_ = false;
};

class Scheduler {
public:
    Scheduler(std::mutex& host_mutex, lv2::Kernel& kernel, u32 spurs, Instance& inst);
    ~Scheduler();

    void start();
    void stop();

    // SpursHost::mutex, which guards `inst`. Workers lock it directly: at the process end the scheduler is destroyed
    // from inside the HLE process's state map, so they must not go through HleProcess::state() then. The mutex
    // outlives the instances (SpursHost destroys its map first).
    std::mutex& host_mutex;
    lv2::Kernel& kernel;
    u8* const base;
    const u32 spurs;
    Instance& inst;
    const bool second; // SPURS2: 32 workloads, kernel 2's selection and exit address
    std::atomic<bool> stopping{false};
    // Workloads the native scheduler does not know (not registered through the HLE API): left out of the selection
    // after the first time a worker picked one, which is logged. Bit `wid`.
    std::atomic<u32> unsupported{0};
    // Where the workers' time went since the last report (D-131): each workload's runs, policy polls that found no
    // work, waits. The first worker reports every 5 s.
    std::array<std::atomic<u64>, CELL_SPURS_MAX_WORKLOAD2> workload_ns{};
    // Workers inside each workload right now (D-136): a join waits for them to leave before the game frees its memory.
    std::array<std::atomic<u32>, CELL_SPURS_MAX_WORKLOAD2> inside{};
    std::atomic<u64> idle_poll_ns{0}, wait_ns{0};
    void report_time(u64 now_ns);
    u64 last_report_ns = 0; // the reporting worker's

private:
    std::vector<std::unique_ptr<Worker>> workers_;
};

// The native taskset policy module (spurs_taskset.cpp): runs the taskset's tasks on `w` until the policy gives the
// SPU back to the kernel (no task to run, or another workload should run).
void run_taskset(Worker& w, u32 wid, u32 taskset, u32 poll_status);
// A game's own policy module (cellSpursAddWorkload; spurs_custom.cpp): runs it on `w` until it returns to the kernel.
void run_custom(Worker& w, u32 wid, u32 poll_status);
// The native job chain policy module (spurs_jobchain.cpp): runs the chain's jobs on `w` until the chain has no work for
// this SPU or another workload should run.
void run_jobchain(Worker& w, u32 wid, u32 job_chain);

// Kernel helpers the policy uses.
// Adds `n` to the workload's ready count in CellSpurs line 0 (clamped to 0..255; a decrement that would go below 0 waits,
// as libsre's policy module does).
void add_ready_count(Worker& w, u32 wid, s32 n);

} // namespace pspocket::hle::spurs
