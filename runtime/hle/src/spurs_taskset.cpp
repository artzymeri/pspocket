// The native taskset policy module (D-037 stage 3b, D-050): runs a taskset's SPU tasks on a worker the way Sony's
// taskset policy module does, without running it. The reference is libsre (fw 4.92, read on the Mac as the reference,
// never copied; D-046); RPCS3's cellSpursSpu.cpp is an unwired draft of the same module and differs from libsre where
// noted.
//
// What a task sees is libsre's contract: the task image loaded at 0x3000.. (the ELF loader's checks), registers at
// entry ($3 the task argument, $4 the taskset's arguments and SPURS address), the kernel and taskset contexts in local
// store, and the syscall entry at 0xA70 ($3 = number, $4 = argument, result in $3): 0 exit, 1 yield, 2 wait for a
// signal, 3 poll, 4 receive the workload flag (+0x10: don't wait for DMA, which is synchronous here). A task that
// blocks or yields is saved to its context save area exactly as libsre saves it (0x380 bytes of execution context, then
// the LS pattern's blocks, packed) and restored from it when it runs again, on any worker.
//
// The task bits live in the guest taskset's first 128-byte line and change only through the reservation system, so
// SPU task code (GETLLAR/PUTLLC) and PPU code (lwarx/stwcx, HLE) see consistent states. After every request the
// workload's ready count in CellSpurs line 0 is updated, as libsre does.
//
// A task whose ELF word has bit 0 (and not bit 2) has an exit callback (D-195): its descriptor (code EA | size in
// 128-byte units) and argument come from the CellSpursTaskset2's table at 0x1980 when it starts, and when it exits
// the callback's code is loaded at 0x10000 and run with the taskset, task ID, exit code and argument (RPCS3
// cellSpursSpu.cpp spursTasksetOnTaskExit, a draft of libsre's; Sony's libvdec chains its H.264 decoder's tasks so).
// libsre's own exit code handler is done natively (store_exit_code). A task with bit 2 is a CellSpursTaskset2's own
// task (cellSpursCreateTask2, D-626): when it exits, libsre's Task2 handler's work is done natively (task2_exit): its
// exit code into the taskset's exit slot, then its bit of the taskset's lv2 event flag, which cellSpursJoinTask2 waits
// on. Elf bit 1 (a debugger break) is logged.

#include <atomic>
#include <bit>
#include <cstring>
#include <format>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/spu/runtime.h"
#include "spu_programs.hpp"
#include "spurs_native.hpp"

namespace pspocket::hle::spurs {
namespace {

constexpr std::string_view kTaskChan = "spurs";
constexpr u32 kBlock = 0x800;
constexpr u32 CELL_SPURS_TASK_ERROR_NOSYS = 0x80410903;

// Task bitsets in the taskset line (128 bits, task 0 the top bit).
namespace bits {
constexpr u32 running = 0x00, ready = 0x10, pending_ready = 0x20, enabled = 0x30, signalled = 0x40, waiting = 0x50,
              wkl_flag_wait_task = 0x72, last_scheduled_task = 0x73;
}

u128 read128(u8* p, u32 off)
{
    return (u128{ps_read64(p, off)} << 64) | ps_read64(p, off + 8);
}
void write128(u8* p, u32 off, u128 v)
{
    ps_write64(p, off, static_cast<u64>(v >> 64));
    ps_write64(p, off + 8, static_cast<u64>(v));
}
u128 task_bit(u32 id) { return u128{1} << (127 - id); }
u32 popcount128(u128 v) { return static_cast<u32>(std::popcount(static_cast<u64>(v >> 64)) + std::popcount(static_cast<u64>(v))); }

// Quadwords between local store (guest byte order) and registers (D-004: host byte j = guest byte 15 - j).
void store_qw(u8* ls, u32 addr, const ps_spu_v128& r)
{
    for (u32 j = 0; j < 16; j++) ls[addr + j] = r.u8[15 - j];
}
ps_spu_v128 load_qw(u8* ls, u32 addr)
{
    ps_spu_v128 r;
    for (u32 j = 0; j < 16; j++) r.u8[15 - j] = ls[addr + j];
    return r;
}
ps_spu_v128 word0(u32 v)
{
    ps_spu_v128 r{};
    r.u32[3] = v; // the preferred slot
    return r;
}

enum class Request : s32 { PollSignal = -1, Destroy = 0, Yield = 1, WaitSignal = 2, Poll = 3, WaitWklFlag = 4, Select = 5,
                           RecvWklFlag = 6 };
enum class Next { Resume, Dispatch, Exit };

class Policy {
public:
    Policy(Worker& w, u32 wid, u32 taskset) : w_(w), wid_(wid), taskset_(taskset), base_(w.sched.base), ls_(w.ls) {}

    void run(u32 poll_status);

private:
    s32 request(Request r, u32* task_id = nullptr, bool* was_waiting = nullptr);
    bool dispatch(); // false: no task to run (the policy exits)
    void start_task(u32 id, u64 elf);
    void resume_task(u32 id, u64 elf);
    void load_elf(u32 id, u32 elf, bool skip_writable, u32* entry, u32* lowest);
    void attach_code(u32 elf); // the lifted program for the task's code (D-076)
    Next syscall();
    s32 save_context();
    Next incident(); // after a task left the SPU: next task, or exit to the kernel
    void return_to_task(u32 result);
    [[noreturn]] void fatal(std::string_view what);
    void run_exit_callback(u32 exit_code);
    void store_exit_code(u32 container, u32 exit_code);
    void task2_exit(u32 exit_code);

    u32 task_id() const { return ps_read32(ls_, ls::ts_task_id); }
    u8* guest(u32 ea) const { return base_ + ea; }

    Worker& w_;
    const u32 wid_;
    const u32 taskset_;
    u8* const base_;
    u8* const ls_;
};

void Policy::fatal(std::string_view what)
{
    PS_FATAL(kTaskChan, "SPURS {:#x} SPU {}, taskset {:#x} (workload {}), task {}: {} (pc 0x{:05x}, lr 0x{:05x})",
        w_.sched.spurs, w_.spu_num, taskset_, wid_, static_cast<s32>(task_id()), what, w_.ctx.pc,
        w_.ctx.gpr[0].u32[3]);
}

// The taskset requests (libsre's policy module, request function): one atomic update of the taskset line, then the
// ready count. Default write-back: ready = ready | pending_ready | signalled, signalled = signalled & (ready |
// pending_ready), pending_ready = 0; `n` counts tasks that became ready (or stopped being ready).
s32 Policy::request(Request r, u32* task_id_out, bool* was_waiting)
{
    const u32 current = task_id();
    const u128 m = current < CELL_SPURS_MAX_TASK ? task_bit(current) : 0;
    s32 rc = 0, n = 0;
    u32 selected = CELL_SPURS_MAX_TASK;
    bool waiting_bit = false;
    bool bad = false;
    reservation_op(base_, taskset_, [&](u8* line) {
        rc = 0;
        selected = CELL_SPURS_MAX_TASK;
        waiting_bit = false;
        u128 R = read128(line, bits::running), Y = read128(line, bits::ready), P = read128(line, bits::pending_ready),
             E = read128(line, bits::enabled), S = read128(line, bits::signalled), W = read128(line, bits::waiting);
        bad = (R & W) || (Y & P) || ((R | Y | P | S | W) & ~E);
        if (bad) return false;
        n = static_cast<s32>(popcount128((S | P) & ~Y));
        u128 ready = Y | P | S, signalled = S & (Y | P);
        u8 flag_task = ps_read8(line, bits::wkl_flag_wait_task);
        u8 last = ps_read8(line, bits::last_scheduled_task);
        u128 avail = ready & ~R;
        if (flag_task < CELL_SPURS_MAX_TASK) avail &= ~task_bit(flag_task);
        switch (r) {
        case Request::PollSignal:
            rc = (signalled & m) ? 1 : 0;
            signalled &= ~m;
            break;
        case Request::Destroy:
            R &= ~m;
            E &= ~m;
            ready &= ~m;
            signalled &= ~m;
            n -= 1;
            break;
        case Request::Yield:
            R &= ~m;
            W |= m;
            break;
        case Request::WaitSignal:
            if (signalled & m) {
                signalled &= ~m;
                rc = 1;
            } else {
                R &= ~m;
                W |= m;
                ready &= ~m;
                n -= 1;
            }
            break;
        case Request::Poll: rc = (avail & ~m) ? 1 : 0; break;
        case Request::WaitWklFlag:
            if (flag_task == 0x81) {
                flag_task = 0x80;
            } else if (flag_task == 0x80) {
                flag_task = static_cast<u8>(current);
                R &= ~m;
                W |= m;
                n -= 1;
                rc = 1;
            } else {
                rc = static_cast<s32>(CELL_SPURS_TASK_ERROR_BUSY);
            }
            break;
        case Request::Select:
            // Round robin from the task after the last one scheduled.
            for (u32 k = 1; k <= CELL_SPURS_MAX_TASK; k++) {
                const u32 t = (last + k) % CELL_SPURS_MAX_TASK;
                if (avail & task_bit(t)) {
                    selected = t;
                    break;
                }
            }
            if (selected < CELL_SPURS_MAX_TASK) {
                waiting_bit = (W & task_bit(selected)) != 0;
                R |= task_bit(selected);
                W &= ~task_bit(selected);
                last = static_cast<u8>(selected);
            }
            break;
        case Request::RecvWklFlag:
            if (flag_task < 0x80) {
                flag_task = 0x80;
                n += 1;
                rc = 1;
            } else {
                flag_task = 0x81;
            }
            break;
        }
        write128(line, bits::running, R);
        write128(line, bits::ready, ready);
        write128(line, bits::pending_ready, 0);
        write128(line, bits::enabled, E);
        write128(line, bits::signalled, signalled);
        write128(line, bits::waiting, W);
        ps_write8(line, bits::wkl_flag_wait_task, flag_task);
        ps_write8(line, bits::last_scheduled_task, last);
        return true;
    });
    if (bad) fatal("inconsistent task bits in the taskset (libsre halts the SPU)");
    // The policy module's copy of the line (LS 0x2700), then the ready count.
    alignas(16) u8 line[PS_RES_LINE_SIZE];
    ps_res_acquire(base_, taskset_, line);
    std::memcpy(ls_ + ls::ts, line, sizeof line);
    add_ready_count(w_, wid_, n);
    if (task_id_out) *task_id_out = selected;
    if (was_waiting) *was_waiting = waiting_bit;
    return rc;
}

// libsre's ELF loader: the header (exact e_ident, EM_SPU, entry above 0x2FFF) and program headers within the first
// 0x300 bytes; PT_LOAD segments (all, or only read-only ones when resuming) copied to their LS address, their bss
// zeroed; the walk stops at the first program header with p_paddr >= 0x40000. libsre halts the SPU on a bad ELF.
void Policy::load_elf(u32 id, u32 elf, bool skip_writable, u32* entry, u32* lowest)
{
    static constexpr u8 kIdent[16] = {0x7F, 'E', 'L', 'F', 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (!elf || elf % 16) fatal(std::format("task {} has ELF address {:#x} (INVAL)", id, elf));
    if (!w_.sched.kernel.memory().is_committed(elf)) fatal(std::format("task {}'s ELF at {:#x} is not mapped", id, elf));
    u8* e = guest(elf);
    if (std::memcmp(e, kIdent, 16) != 0 || ps_read16(e, 0x12) != 0x17 || ps_read32(e, 0x18) <= 0x2FFF) {
        fatal(std::format("task {}'s ELF at {:#x} is not an SPU task ELF (NOEXEC)", id, elf));
    }
    const u32 phoff = ps_read32(e, 0x1C), phentsize = ps_read16(e, 0x2A), phnum = ps_read16(e, 0x2C);
    if (phoff + phentsize * phnum > 0x300) {
        fatal(std::format("task {}'s ELF at {:#x} has program headers past 0x300 (NOMEM)", id, elf));
    }
    u32 low = ls::size;
    for (u32 i = 0; i < phnum; i++) {
        u8* ph = e + phoff + i * phentsize;
        if (ps_read32(ph, 0x0C) >= ls::size) break; // p_paddr
        if (ps_read32(ph, 0x00) != 1) continue;     // PT_LOAD
        if (skip_writable && (ps_read32(ph, 0x18) & 2)) continue;
        const u32 offset = ps_read32(ph, 0x04), vaddr = ps_read32(ph, 0x08), filesz = ps_read32(ph, 0x10),
                  memsz = ps_read32(ph, 0x14);
        if (vaddr < ls::task_top || u64{vaddr} + memsz > ls::size || filesz > memsz) {
            fatal(std::format("task {}'s segment {:#x}+{:#x} is outside the task area (FAULT)", id, vaddr, memsz));
        }
        std::memcpy(ls_ + vaddr, guest(elf + offset), filesz);
        std::memset(ls_ + vaddr + filesz, 0, memsz - filesz);
        low = std::min(low, vaddr);
    }
    if (entry) *entry = ps_read32(e, 0x18);
    if (lowest) *lowest = low;
}

// The task's code is the executable PT_LOAD segment of its ELF that holds the entry (load_elf checked the headers),
// else its first executable one: attach the lifted program for it, if there is one (D-076). An ELF with SPU overlays
// starts with a small executable segment before the one it enters (the firmware's encoders and video decoder, D-190);
// lv2's SPU threads take the segment holding the entry too (sys_spu_thread_group_start).
void Policy::attach_code(u32 elf)
{
    u8* e = guest(elf);
    const u32 phoff = ps_read32(e, 0x1C), phentsize = ps_read16(e, 0x2A), phnum = ps_read16(e, 0x2C);
    const u32 entry = ps_read32(e, 0x18);
    u8* code = nullptr;
    for (u32 i = 0; i < phnum; i++) {
        u8* ph = e + phoff + i * phentsize;
        if (ps_read32(ph, 0x0C) >= ls::size) break;
        if (ps_read32(ph, 0x00) != 1 || !(ps_read32(ph, 0x18) & 1)) continue; // PT_LOAD with PF_X
        if (!code) code = ph;
        if (entry >= ps_read32(ph, 0x08) && entry - ps_read32(ph, 0x08) < ps_read32(ph, 0x10)) {
            code = ph;
            break;
        }
    }
    if (!code) {
        ps_spu_attach_program(&w_.ctx, nullptr);
        return;
    }
    attach_program(w_.ctx, base_, {ImageKind::Task, elf + ps_read32(code, 0x04), ps_read32(code, 0x08),
                                      ps_read32(code, 0x10) & ~3u, entry});
}

// A task starts (libsre: its image loaded, the execution context initialized, then the registers). Local store is not
// cleared (RPCS3 clears 0x3000..0x40000 here): only each segment's bss is zeroed.
void Policy::start_task(u32 id, u64 elf)
{
    const u32 flags = static_cast<u32>(elf & 7);
    if ((flags & 4) && ps_read32(base_, taskset_ + ts_off::size) <= 0x28FF)
        fatal(std::format("task {} is a CellSpursTaskset2 task (ELF word {:#x}) in a taskset that is not one", id, elf));
    if (flags & 2) PS_LOG_WARN(kTaskChan, "task {} of taskset {:#x} asks for a debugger break at dispatch; ignored", id, taskset_);
    u32 entry = 0, lowest = 0;
    ps_write32(ls_, ls::ctx_guid, ls::task_top);
    forget_module_runs(w_.ctx); // a new image: the modules another task loaded are gone
    load_elf(id, static_cast<u32>(elf & ~7ull), false, &entry, &lowest);
    attach_code(static_cast<u32>(elf & ~7ull));
    store_qw(ls_, ls::ctx_lr, word0(entry));
    ps_write32(ls_, ls::ctx_guid, lowest);
    ps_write32(ls_, ls::ctx_mgmt, ls::ts);
    ps_write64(ls_, ls::ctx_exit_cb, 0);
    ps_write32(ls_, ls::ctx_exit_code, 0);
    ps_write32(ls_, ls::ctx_flags, flags & 5);
    if ((flags & 5) == 1) { // the exit callback: descriptor and argument from the CellSpursTaskset2's table
        if (ps_read32(base_, taskset_ + ts_off::size) <= 0x28FF)
            fatal(std::format("task {} has an exit callback (ELF word {:#x}) in a taskset that is not a CellSpursTaskset2",
                id, elf));
        std::memcpy(ls_ + ls::ctx_exit_cb, guest(taskset_ + kTaskExitCallbacks + 16 * id), 16);
        PS_LOG_DEBUG(kTaskChan, "task {} of taskset {:#x}: exit callback {:#x}, argument {:#x}", id, taskset_,
            ps_read64(ls_, ls::ctx_exit_cb), ps_read64(ls_, ls::ctx_exit_arg));
    }

    // The first 8 and every 1024th: in its first mission GTA V starts thousands of tasks a second.
    static std::atomic<u64> starts{0};
    if (const u64 n = ++starts; n <= 8 || n % 1024 == 0) {
        PS_LOG_INFO(kTaskChan, "SPURS {:#x} SPU {}: task {} of taskset {:#x} (workload {}) starts: ELF {:#x}, entry "
                               "0x{:05x} (task start {})",
            w_.sched.spurs, w_.spu_num, id, taskset_, wid_, static_cast<u32>(elf & ~7ull), entry, n);
    }
    SPUContext& c = w_.ctx;
    for (auto& r : c.gpr) r = ps_spu_v128{};
    c.gpr[0] = word0(entry);
    c.gpr[1] = word0(ls::pm_stack);
    c.gpr[3] = load_qw(ls_, ls::ts_task_info); // the 16-byte task argument
    c.gpr[4].u64[1] = ps_read64(base_, taskset_ + ts_off::args); // preferred doubleword: the taskset's arguments
    c.gpr[4].u64[0] = ps_read64(base_, taskset_ + ts_off::spurs);
    c.ch.tag_mask = 1u << 31;
    c.pc = entry;
}

// A task that left with its context saved resumes (libsre): the read-only segments reloaded unless the pattern keeps
// all of local store, the execution context and the pattern's blocks restored from the save area, then it continues
// after its syscall with $3 = 0.
void Policy::resume_task(u32 id, u64 elf)
{
    if (ps_read8(base_, taskset_ + ts_off::enable_clear_ls)) std::memset(ls_ + ls::task_top, 0, ls::size - ls::task_top);
    const u128 pattern = read128(ls_, ls::ts_ti_pattern);
    constexpr u128 kAllBlocks = (u128{0x03FFFFFFFFFFFFFFull} << 64) | 0xFFFFFFFFFFFFFFFFull;
    if (pattern != kAllBlocks) load_elf(id, static_cast<u32>(elf & ~7ull), true, nullptr, nullptr);
    attach_code(static_cast<u32>(elf & ~7ull));
    const u32 save = static_cast<u32>(ps_read64(ls_, ls::ts_ti_context) & ~0x7Full);
    if (!save) fatal("a waiting task has no context save area");
    std::memcpy(ls_ + ls::ctx_area, guest(save), ls::ctx_area_size);
    u32 k = 0;
    for (u32 b = 6; b < 128; b++) {
        if (pattern & task_bit(b)) std::memcpy(ls_ + b * kBlock, guest(save + 0x400 + kBlock * k++), kBlock);
    }
    // The modules the task had loaded come back with its blocks (D-082).
    forget_module_runs(w_.ctx);
    restore_module_runs(w_.ctx, save);
    SPUContext& c = w_.ctx;
    const ps_spu_v128 fpscr = load_qw(ls_, ls::ctx_fpscr);
    ps_spu_fscrwr(&c, &fpscr);
    c.ch.tag_mask = ps_read32(ls_, ls::ctx_tag_mask);
    c.ch.event_mask = ps_read32(ls_, ls::ctx_event_mask);
    for (auto& r : c.gpr) r = ps_spu_v128{};
    return_to_task(0);
}

// The syscall entry's epilogue: $0, $1 and $80..$127 from the execution context, $3 = the result, back after the call.
void Policy::return_to_task(u32 result)
{
    SPUContext& c = w_.ctx;
    c.gpr[0] = load_qw(ls_, ls::ctx_lr);
    c.gpr[1] = load_qw(ls_, ls::ctx_sp);
    for (u32 i = 0; i < 48; i++) c.gpr[80 + i] = load_qw(ls_, ls::ctx_r80 + 16 * i);
    c.gpr[3] = word0(result);
    c.pc = c.gpr[0].u32[3] & PS_SPU_PC_MASK;
}

// Picks the next task and starts or resumes it; false when there is none.
bool Policy::dispatch()
{
    u32 id = CELL_SPURS_MAX_TASK;
    bool was_waiting = false;
    request(Request::Select, &id, &was_waiting);
    if (id >= CELL_SPURS_MAX_TASK) return false;
    ps_write32(ls_, ls::ts_task_id, id);
    const u32 info = taskset_ + ts_off::task_info + id * ts_off::task_info_stride;
    std::memcpy(ls_ + ls::ts_task_info, guest(info), ts_off::task_info_stride);
    const u64 elf = ps_read64(base_, info + ts_off::ti_elf);
    ps_write64(ls_, ls::ts_task_info + ts_off::ti_elf, elf & ~7ull);
    if (was_waiting) {
        resume_task(id, elf);
    } else {
        start_task(id, elf);
    }
    return true;
}

// libsre's context save: STAT without a save area, with more pattern blocks than the area holds (libsre counts from
// block 8), or when the stack (from the saved $sp up) is not all in the pattern. Then FPSCR and the two channel masks
// go into the execution context, which is written out with the pattern's blocks, packed.
s32 Policy::save_context()
{
    const u64 context = ps_read64(ls_, ls::ts_ti_context);
    const u32 save = static_cast<u32>(context & ~0x7Full), allocated = static_cast<u32>(context & 0x7F);
    const u128 pattern = read128(ls_, ls::ts_ti_pattern);
    if (!save) return error(CELL_SPURS_TASK_ERROR_STAT);
    const u128 from_block_8 = pattern & ((u128{1} << 120) - 1);
    if (popcount128(from_block_8) > allocated) return error(CELL_SPURS_TASK_ERROR_STAT);
    const u32 sp = ps_read32(ls_, ls::ctx_sp) & (ls::size - 1);
    for (u32 b = sp / kBlock; b < 128; b++) {
        if (!(pattern & task_bit(b))) return error(CELL_SPURS_TASK_ERROR_STAT);
    }
    SPUContext& c = w_.ctx;
    store_qw(ls_, ls::ctx_fpscr, c.fpscr);
    ps_write32(ls_, ls::ctx_tag_mask, c.ch.tag_mask);
    ps_write32(ls_, ls::ctx_event_mask, c.ch.event_mask);
    std::memcpy(guest(save), ls_ + ls::ctx_area, ls::ctx_area_size);
    u32 k = 0;
    for (u32 b = 6; b < 128; b++) {
        if (pattern & task_bit(b)) std::memcpy(guest(save + 0x400 + kBlock * k++), ls_ + b * kBlock, kBlock);
    }
    ps_res_notify_write(save, 0x400 + kBlock * k);
    // The modules it loaded go with the blocks that hold them (D-082): restoring them is not a DMA the tracker sees.
    save_module_runs(w_.ctx, save, [&pattern](u32 ls_start, u32 ls_end) {
        for (u32 b = ls_start / kBlock; b <= (ls_end - 1) / kBlock; b++)
            if (b < 6 || !(pattern & task_bit(b))) return false;
        return true;
    });
    return lv2::CELL_OK;
}

// An exiting task's callback (RPCS3 spursTasksetOnTaskExit): its code (EA | size in 128-byte units, as libsre's PPU
// side encodes the descriptors it writes) loaded at 0x10000 and called with $3 the taskset, $4 the task ID, $5 the
// exit code and $6 the argument; it returns to a `stop` of its own. libsre's own handlers are markers here (spurs.hpp):
// those are not implemented.
void Policy::run_exit_callback(u32 exit_code)
{
    const u32 desc = static_cast<u32>(ps_read64(ls_, ls::ctx_exit_cb));
    const u32 ea = desc & ~0x7Fu, size = (desc & 0x7F) * 128;
    if (ea == SPURS_IMG_ADDR_TASK_EXIT_CODE_HANDLER) {
        store_exit_code(static_cast<u32>(ps_read64(ls_, ls::ctx_exit_arg)), exit_code);
        return;
    }
    if (ea == SPURS_IMG_ADDR_TASK2_HANDLER)
        fatal(std::format("task {}: libsre's CellSpursTaskset2 task handler ({:#x}) is not implemented", task_id(), desc));
    if (!size || ls::exit_callback + size > ls::size) fatal(std::format("task {}: exit callback {:#x}", task_id(), desc));
    std::memcpy(ls_ + ls::exit_callback, guest(ea), size);
    attach_program(w_.ctx, base_, {ImageKind::Task, ea, ls::exit_callback, size, ls::exit_callback});
    SPUContext& c = w_.ctx;
    c.gpr[0] = word0(ls::exit_callback_return);
    c.gpr[3] = ps_spu_v128{};
    c.gpr[3].u64[1] = taskset_;
    c.gpr[4] = word0(task_id());
    c.gpr[5] = word0(exit_code);
    c.gpr[6] = ps_spu_v128{};
    c.gpr[6].u64[1] = ps_read64(ls_, ls::ctx_exit_arg);
    c.pc = ls::exit_callback;
    ps_write32(ls_, ls::exit_callback_return, kExitCallbackStop);
    PS_LOG_DEBUG(kTaskChan, "task {} of taskset {:#x}: exit callback {:#x} ({:#x} bytes), exit code {:#x}", task_id(),
        taskset_, ea, size, exit_code);
    const u32 reason = ps_spu_run(&c);
    if (reason == PS_SPU_STOP_REQUESTED) return;
    if (reason != PS_SPU_STOP_SIGNAL || c.stop_code != kExitCallbackStop || c.pc != ls::exit_callback_return + 4)
        fatal(std::format("task {}: its exit callback {:#x} stopped (reason {}, code {:#x}) instead of returning", task_id(),
            ea, reason, c.stop_code));
}

// libsre's exit code handler (the SPU code its CellSpursTaskset2 keeps at 0x1900, D-195), natively: the exit code goes
// into the task's exit code container, one 128-byte line: the status word at +0x20 (bit 0 initialized, bit 2 an exit
// code stored, bit 16 a PPU thread waits, bit 17 an SPU task waits), the exit code at +0x28, a waiting task's taskset
// (u64) at +0x30 and its ID (a byte) at +0x38. A NULL or misaligned container, or one not initialized, is left alone (libsre's
// handler returns an error nobody reads). Then the task that waits is signalled. A PPU thread waiting in pspocket's
// cellSpursTaskExitCodeGet sleeps until the container's line changes, which this update does (D-627), and marks
// nothing; bit 16 (libsre's PPU waiter, with an event flag at +0x40) is set by no native code: it stops loudly.
void Policy::store_exit_code(u32 container, u32 exit_code)
{
    if (!container || (container & 0x7F)) {
        PS_LOG_ERROR(kTaskChan, "task {} of taskset {:#x}: exit code container {:#x} is NULL or misaligned", task_id(),
            taskset_, container);
        return;
    }
    u32 old = 0;
    bool stored = false;
    reservation_op(base_, container, [&](u8* line) {
        old = ps_read32(line, 0x20);
        stored = old & 1;
        if (!stored) return false;
        ps_write32(line, 0x20, old | 4);
        ps_write32(line, 0x28, exit_code);
        return true;
    });
    if (!stored) {
        PS_LOG_ERROR(kTaskChan, "task {} of taskset {:#x}: exit code container {:#x} is not initialized", task_id(), taskset_,
            container);
        return;
    }
    PS_LOG_DEBUG(kTaskChan, "task {} of taskset {:#x}: exit code {:#x} into container {:#x} (status {:#x})", task_id(),
        taskset_, exit_code, container, old);
    if (old & (1u << 17)) {
        const u64 waiter_taskset = ps_read64(base_, container + 0x30);
        const u32 waiter = base_[container + 0x38];
        if (const s32 rc = send_signal(base_, static_cast<u32>(waiter_taskset), waiter); rc != lv2::CELL_OK)
            PS_LOG_ERROR(kTaskChan, "exit code container {:#x}: signalling task {} of taskset {:#x}: {:#x}", container,
                waiter, waiter_taskset, static_cast<u32>(rc));
    }
    if (old & (1u << 16)) fatal(std::format("exit code container {:#x}: a PPU thread waiting on it is not implemented", container));
}

// libsre's handler for a CellSpursTaskset2's own tasks (the SPU code whose descriptor cellSpursCreateTaskset2 keeps at
// 0x78; D-626), natively, after the task's destroy request: its exit code into the first word of the task's exit slot
// in the taskset (0x1980 + 16 * id), in one update of the slot's line, then its bit (id % 64) of the taskset's lv2
// event flag for its half (0x1898 for tasks 0..63, 0x189C for 64..127), which cellSpursJoinTask2 waits on. The task's
// ID stays out of reach of new tasks until it is joined (cellSpurs.cpp keeps that). A flag that does not exist (the
// taskset not made by cellSpursCreateTaskset2, or destroyed) is logged: nobody can join the task then.
void Policy::task2_exit(u32 exit_code)
{
    const u32 id = task_id();
    const u32 slot = taskset_ + ts_off::exit_slots + 16 * id;
    reservation_op(base_, slot, [&](u8* line) {
        ps_write32(line, slot & (PS_RES_LINE_SIZE - 1), exit_code);
        return true;
    });
    const u32 flag_id = ps_read32(base_, taskset_ + task2_flag_offset(id));
    if (const auto flag = w_.sched.kernel.event_flag(flag_id)) {
        flag->set(task2_flag_bit(id));
    } else {
        PS_LOG_ERROR(kTaskChan, "task {} of CellSpursTaskset2 {:#x} exited ({:#x}), but its lv2 event flag {:#x} does not "
            "exist: no cellSpursJoinTask2 can take its exit code", id, taskset_, exit_code, flag_id);
    }
    PS_LOG_DEBUG(kTaskChan, "task {} of CellSpursTaskset2 {:#x}: exit code {:#x}, event flag {:#x} bit {}", id, taskset_,
        exit_code, flag_id, id & 63);
}

// A task gave up the SPU (exit, yield, wait): its GUID in local store is cleared (the saved copy keeps it), then the
// next task runs unless another workload should have the SPU.
Next Policy::incident()
{
    std::memset(ls_ + ps_read32(ls_, ls::ctx_guid), 0, 16);
    if (w_.poll_switch()) return Next::Exit;
    return Next::Dispatch;
}

Next Policy::syscall()
{
    SPUContext& c = w_.ctx;
    // The syscall entry's prologue: $0, $1, $80..$127 into the execution context.
    store_qw(ls_, ls::ctx_lr, c.gpr[0]);
    store_qw(ls_, ls::ctx_sp, c.gpr[1]);
    for (u32 i = 0; i < 48; i++) store_qw(ls_, ls::ctx_r80 + 16 * i, c.gpr[80 + i]);
    const u32 number = c.gpr[3].u32[3], arg = c.gpr[4].u32[3];
    const u32 call = (number & 0x10) ? (number & 0x0F) : number;
    switch (call) {
    case 0: { // exit: the code is at 0x2FD0 (cellSpursTaskExit put it there)
        // A CellSpursTaskset2 task by its TaskInfo's ELF word, read before the destroy request frees the ID (the
        // execution context's flags at 0x2FD4 say the same, but sit in the quadword a task stores its exit code in).
        const u64 elf = ps_read64(base_, taskset_ + ts_off::task_info + task_id() * ts_off::task_info_stride + ts_off::ti_elf);
        request(Request::Destroy);
        if (elf & 4) task2_exit(ps_read32(ls_, ls::ctx_exit_code));
        else if (static_cast<u32>(ps_read64(ls_, ls::ctx_exit_cb))) run_exit_callback(ps_read32(ls_, ls::ctx_exit_code));
        PS_LOG_DEBUG(kTaskChan, "SPURS {:#x}: task {} of taskset {:#x} exited ({:#x})", w_.sched.spurs, task_id(), taskset_,
            ps_read32(ls_, ls::ctx_exit_code));
        return incident();
    }
    case 1: { // yield: switch only if another task of the taskset, or another workload, wants the SPU
        if (!w_.poll_switch() && !request(Request::Poll)) {
            return_to_task(0);
            return Next::Resume;
        }
        if (const s32 rc = save_context(); rc != lv2::CELL_OK) {
            return_to_task(static_cast<u32>(rc));
            return Next::Resume;
        }
        request(Request::Yield);
        return incident();
    }
    case 2: { // wait for a signal
        if (request(Request::PollSignal) == 1) {
            return_to_task(0);
            return Next::Resume;
        }
        if (const s32 rc = save_context(); rc != lv2::CELL_OK) {
            return_to_task(static_cast<u32>(rc));
            return Next::Resume;
        }
        if (request(Request::WaitSignal) == 1) { // the signal came meanwhile: no switch (the context was saved anyway)
            return_to_task(0);
            return Next::Resume;
        }
        PS_LOG_DEBUG(kTaskChan, "SPURS {:#x}: task {} of taskset {:#x} waits (on {:#x})", w_.sched.spurs, task_id(), taskset_,
            ps_read64(ls_, ls::ctx_wait_object));
        return incident();
    }
    case 3: { // poll: bit 0 another task is ready, bit 1 another workload wants the SPU
        const u32 rc = (w_.poll_switch() ? 2u : 0u) | (request(Request::Poll) ? 1u : 0u);
        return_to_task(rc);
        return Next::Resume;
    }
    case 4: { // receive the workload flag
        if (!arg) fatal("syscall 4 (receive the workload flag) with a zero argument (libsre halts the SPU)");
        const bool other = w_.poll_switch();
        const s32 rc = request(Request::WaitWklFlag);
        if (!other && rc != 1) {
            return_to_task(static_cast<u32>(rc));
            return Next::Resume;
        }
        if (const s32 sr = save_context(); sr != lv2::CELL_OK) {
            return_to_task(static_cast<u32>(sr));
            return Next::Resume;
        }
        return incident();
    }
    default:
        return_to_task(CELL_SPURS_TASK_ERROR_NOSYS);
        return Next::Resume;
    }
}

void Policy::run(u32 poll_status)
{
    // Policy module entry: the kernel context's module ID, the taskset context, the syscall entry.
    ps_write16(ls_, ls::k_module_id, 0x544B); // 'TK'
    ps_write64(ls_, ls::ts_taskset, taskset_);
    ps_write32(ls_, ls::ts_kernel, ls::kernel);
    ps_write32(ls_, ls::ts_syscall, ls::pm_syscall);
    ps_write32(ls_, ls::ts_spu_num, w_.spu_num);
    ps_write32(ls_, ls::ts_dma_tag, 31);
    ps_write32(ls_, ls::ts_task_id, 0xFFFFFFFF);
    std::memcpy(ls_ + ls::ts_module_id, "SPURSTASK MODULE", 16);
    ps_write32(ls_, ls::pm_syscall, kSyscallStop); // `stop`: the worker takes the call
    if (poll_status & 4) request(Request::RecvWklFlag);

    Next next = dispatch() ? Next::Resume : Next::Exit;
    while (next != Next::Exit) {
        if (next == Next::Dispatch) {
            if (!dispatch()) break;
        }
        const u32 reason = ps_spu_run(&w_.ctx);
        if (reason == PS_SPU_STOP_REQUESTED) break; // the scheduler is stopping
        if (reason == PS_SPU_STOP_SIGNAL && w_.ctx.stop_code == kSyscallStop && w_.ctx.pc == ls::pm_syscall + 4) {
            next = syscall();
            continue;
        }
        switch (reason) {
        case PS_SPU_STOP_SIGNAL:
            fatal(std::format("stopped with code {:#x}{}", w_.ctx.stop_code, w_.ctx.stop_code == 0x3FFF ? " (stopd: an "
                "assertion in the task)" : ""));
        case PS_SPU_STOP_HALT: fatal("halted");
        case PS_SPU_STOP_EXCEPTION:
            fatal(std::format("SPU exception {} (detail {:#x})", w_.ctx.exception, w_.ctx.stop_code));
        default: fatal(std::format("stopped (reason {})", reason));
        }
    }
    // Policy module exit: no module ID while the kernel runs.
    ps_write16(ls_, ls::k_module_id, 0);
}

} // namespace

void run_taskset(Worker& w, u32 wid, u32 taskset, u32 poll_status)
{
    Policy(w, wid, taskset).run(poll_status);
}

} // namespace pspocket::hle::spurs
