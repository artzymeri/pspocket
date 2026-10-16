#pragma once

// Thread and wait reports: what each PPU thread is doing or waiting for, where it is in guest code, and the state of
// the RSX's FIFO. pspocket-boot prints one when a run ends at its timeout or on a guest fault (and periodically with
// --report-every), so a game that stops making progress says what it waits on.
//
// Everything is a snapshot taken while the threads run. A blocked thread's registers are exact: it wrote them before
// it parked and Waiter::begin_wait publishes them (cia is its `sc` or the HLE import stub it is in, LR the return
// into its caller). So are those of a thread inside a syscall. A running thread's registers and stack are read while
// it runs, and the report says so. Every guest word is checked before it is read, so a garbage stack ends the
// backtrace, never the report.

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/types.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/object.hpp"

namespace pspocket::lv2 {

struct ThreadSnapshot {
    enum class State : u8 {
        NotStarted, // created; sys_ppu_thread_start not called yet
        Running,    // guest code, or host code of an HLE function that is not waiting on an lv2 object
        Syscall,    // inside a syscall without being parked (file I/O, a busy kernel path)
        Blocked,    // parked: see `wait` (WaitKind::Sleep: sys_timer_usleep)
        Exited,     // exited, not joined yet
    };

    u32 id = 0;
    std::string name;
    s32 priority = 0;
    State state = State::Running;
    bool attached = false;   // a host thread with an lv2 identity: no guest stack, its registers are not read
    bool interrupt = false;  // an interrupt thread (sys_interrupt_thread_establish): waits for its interrupt, not on lv2
    bool current = false;    // the thread that took the snapshot (its registers are exact)
    u32 syscall = kNoSyscall; // the syscall it is in; kNoSyscall in guest code and in HLE functions
    u64 exit_status = 0;     // Exited

    // Blocked.
    WaitOn wait;
    std::chrono::nanoseconds waited{0};     // how long it has been blocked
    std::chrono::nanoseconds remaining{-1}; // until its timeout; negative: no timeout
    std::string waits_on; // the object and its state ("event queue 0x8d000100: 0 of 16 events queued, 1 waiter")

    // Guest position (not for attached threads; NotStarted: cia is the entry). `racy`: read while the thread ran.
    bool registers = false;
    bool racy = false;
    u32 cia = 0;
    u64 lr = 0;
    u64 sp = 0;
    std::vector<u32> backtrace; // cia, then return addresses from LR and the stack back chain, innermost first
};

// Every live PPU thread (including zombies and threads not started yet), sorted by ID.
std::vector<ThreadSnapshot> snapshot_threads(Kernel& kernel, usize max_frames = 12);

// A PPC64 stack walk: `cia`, then `lr` when it is a code address, then the LR save word ([frame + 16]) of each frame
// up the back chain ([sp], [[sp]], ...), at most `max_frames` addresses. The innermost function's LR, when the back
// chain also has it (the function had saved it), is listed once. The walk stops at the first frame that is not
// 16-byte aligned, does not climb, leaves [stack_lo, stack_hi) (when stack_hi != 0) or is not committed memory, and
// at the first return address that is not a 4-byte aligned address in committed memory (the thread entry's 0).
std::vector<u32> guest_backtrace(const GuestMemory& memory, u32 cia, u64 lr, u64 sp, u32 stack_lo, u32 stack_hi,
    usize max_frames);

struct ReportOptions {
    std::string title; // for the header line, e.g. "timeout after 60.0 s"
    usize max_frames = 12;
    // Names a guest code address, or returns "" (pspocket-boot: the HLE function behind an import stub).
    std::function<std::string(u32 addr)> describe_address;
};

// The report as text: a header with counts, one block per thread (ID, name, priority, state and where; what it waits
// on; its backtrace), then the RSX's FIFO (get, put, words, flips, and what it is blocked on).
std::string format_report(Kernel& kernel, const ReportOptions& options = {});

} // namespace pspocket::lv2
