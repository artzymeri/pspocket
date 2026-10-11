// cellFiber: libfiber.sprx's 47 functions (fw 4.92) as pspocket's own, read from Sony's code (the reference, D-046)
// and compared with it running on pspocket (tests/hle/test_fiber_compare.cpp, 0 differences: D-914; no longer
// firmware-first: a game gets this library, with the system software installed or not). Written in
// D-537's session from libfiber; D-914 put it in place of D-568's model (guest fibers on host threads passing a baton,
// made without Sony's module), which differed from libfiber in its checks, its joins, the worker control's states and
// the attributes. RPCS3's rpcs3/Emu/Cell/Modules/cellFiber.cpp registers them as todo stubs; its cellFiber.h gives the
// structures' sizes.
//
// libfiber has two layers, both laid out in the game's memory exactly where libfiber lays them out:
// - contexts (CellFiberPpuContext, 640 bytes): a state word (0 finalized, 1 initialized, 2 running, 3 suspended), the
//   entry, its argument and stack top until the first run, then the registers a suspended context keeps (its save
//   area at +0x10: r1, r2, r14-r31, LR, CR, f14-f31, VRSAVE, v20-v31), the name, the stack and its overflow marker (four
//   words 0xDEADBEAF at its base). A PPU thread runs a context (cellFiberPpuContextRun) or a scheduler function that
//   picks one (cellFiberPpuContextRunScheduler) on a pseudo-context in its own stack frame; per thread, libfiber's
//   TLS block (the game's TLS variable, _cellFiberPpuInitialize) holds that pseudo-context, the current context, the
//   one switched from, the thread's stack pointer, the cause given to cellFiberPpuContextReturnToThread and the
//   current fiber;
// - fibers (CellFiberPpu, 896 bytes, a context inside) on a scheduler (512 bytes): four priorities of run queues, a
//   lock-free list per priority for fibers that other threads make runnable, the waiting flags, all fibers in a ring,
//   a mutex that is a word and an lv2 semaphore ("_fibRt": libfiber's no-ordering mutex), traces. A fiber's yield,
//   wait (signal, flag, join) and exit enter libfiber's scheduler function on the thread's stack, which picks the
//   next fiber (or goes back to the thread when none is runnable); cellFiberPpuRunFibers runs it on a thread. The
//   utility worker control (768 bytes) runs a scheduler on several threads, sleeping on an event flag ("_fibUt").
//
// A context switch is pspocket's: the registers of the context that stops go into its save area as libfiber's
// setjmp would put them (the caller's values, with r1 the frame libfiber's code would be in), with LR the address of
// one of this module's internal functions (a resume stub) that finishes the call that stopped (its outputs and the
// rest of libfiber's function) and returns to the caller. The registers of the context that goes on are loaded and
// the thread goes back to its innermost guest entry (lv2::run_guest) by the non-local jump of D-161/D-213, which goes
// on at LR: a resume stub, or for a context that never ran a start stub that calls its entry on its stack. So the host
// stack never grows with switches, a fiber can go on on another thread (the utility worker control moves them), and
// every word of the switch stays in guest memory. Frames are reserved below the caller's r1 as libfiber's functions
// reserve them, so a pseudo-context, a waiting record or a scheduler function's stack is where libfiber puts it.
// The library is reached through guest calls (a game's import stubs, call_guest): a switch needs a guest entry below.
//
// Deviations: libfiber's assertions (its "PS3 SDK INTERNAL ASSERTION FAILURE" and abort) are fatal logs with the same
// text; a context entry that returns (libfiber's code after it is a zero word: an illegal instruction) is a fatal log.
// Objects that are not guest memory are refused with CELL_FIBER_ERROR_INVAL and the call site logged, where libfiber
// would fault. libfiber registers user traces with cellLibprof when that module is loaded; pspocket has no
// cellLibprof, so when sys_prx_get_module_id_by_name finds one the traces are logged as not connected. A fiber that
// waits inside another of pspocket's libraries (cellSync2's or cellSpursJq's waits) goes back to that library through
// its guest entry (call_guest), so it must be resumed on the same thread, after the fibers that waited inside it
// later (as with libfiber itself on pspocket).

#include <atomic>
#include <bit>
#include <cstring>
#include <format>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>

#include "guest.hpp"
#include "libc_guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/core/reservation.h"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/ppu/runtime.h"

namespace pspocket::hle {
using libc::guest_range;
using libc::guest_strnlen;
namespace {

constexpr std::string_view kChan = "cellFiber";
constexpr std::string_view kModule = "cellFiber";

enum : u32 {
    CELL_FIBER_ERROR_AGAIN = 0x80760001,
    CELL_FIBER_ERROR_INVAL = 0x80760002,
    CELL_FIBER_ERROR_NOSYS = 0x80760003, // the worker control's event queue connection (libfiber's own code)
    CELL_FIBER_ERROR_NOMEM = 0x80760004,
    CELL_FIBER_ERROR_DEADLK = 0x80760008,
    CELL_FIBER_ERROR_PERM = 0x80760009,
    CELL_FIBER_ERROR_BUSY = 0x8076000A,
    CELL_FIBER_ERROR_ABORT = 0x8076000C,
    CELL_FIBER_ERROR_STAT = 0x8076000F,
    CELL_FIBER_ERROR_ALIGN = 0x80760010,
    CELL_FIBER_ERROR_NULL_POINTER = 0x80760011,
    CELL_FIBER_ERROR_NOSYSINIT = 0x80760020,
};
s32 err(u32 code) { return static_cast<s32>(code); }

// lv2 answers libfiber tolerates from its event flag calls.
constexpr u32 kLv2Busy = 0x8001000A, kLv2Timeout = 0x8001000B;
// sys_prx_get_module_id_by_name's answer for a module that is not loaded.
constexpr u32 kPrxUnknownModule = 0x8001112E;

constexpr u32 kLibraryVersion = 0x492000; // as libfiber's assertions print it

// The attributes' magic words (their first doubleword).
constexpr u64 kContextAttrMagic = 0x992D3B630B7DF290ull, kSchedulerAttrMagic = 0x44007515864E5F4Cull,
              kFiberAttrMagic = 0xF784E782F9EA9359ull;
constexpr u32 kStackMarker = 0xDEADBEAF;
constexpr char kNoName[] = "*NO NAME*";

// ----------------------------------------------------------------------------------------------------- layouts

// CellFiberPpuContext (640 bytes, 16-aligned).
namespace ctx_off {
constexpr u32 state = 0x000, save = 0x010, entry = 0x010, arg = 0x018, top = 0x020, debugger = 0x220, name = 0x228,
              stack = 0x248, stack_size = 0x24C, entry_copy = 0x250, sdk = 0x254, arg_copy = 0x258;
} // namespace ctx_off
enum : u32 { kFinalized = 0, kInitialized = 1, kRunning = 2, kSuspended = 3 };

// A save area (libfiber's setjmp layout).
namespace save_off {
constexpr u32 r1 = 0x00, r2 = 0x08, gpr = 0x18 /* r14..r31 */, lr = 0xA8, cr = 0xB0, fpr = 0xB8 /* f14..f31 */,
              vrsave = 0x148, vr = 0x150 /* v20..v31 */;
}

// libfiber's TLS block, at r13 + the offset _cellFiberPpuInitialize keeps.
namespace tls_off {
constexpr u32 thread = 0x00, current = 0x04, from = 0x08, thread_sp = 0x0C, cause = 0x10, fiber = 0x20, w24 = 0x24,
              running = 0x28, w2c = 0x2C;
}

// CellFiberPpuScheduler (512 bytes, 128-aligned); the worker control is one with 256 bytes more.
namespace sch_off {
constexpr u32 pending = 0x00, threads = 0x40 /* byte: threads in RunFibers */, initialized = 0x41, nonempty = 0x48,
              head = 0x80, tail = 0x90, mutex = 0xA0, mutex2 = 0xC0, ring_e0 = 0xE0, fibers = 0xE8, flags = 0xF8,
              interval = 0x138, last_check = 0x140, auto_check = 0x148, trace = 0x150, trace_mode = 0x154,
              trace_flags = 0x156, name_trace = 0x158, user_trace0 = 0x160, user_trace1 = 0x164, debugger = 0x168;
}
namespace uwc_off {
constexpr u32 word = 0x200, flag = 0x208, interval = 0x280, polling = 0x284, workers = 0x288, sdk = 0x28C;
}
// The worker control's word: worker count (bits 63..56), pollers (55..48), wake-up (47..40), polling (bit 39),
// initialized (bit 38), the sleeping workers' event flag bits (31..0).
constexpr u64 kUwcInitialized = 0x4000000000ull, kUwcPolling = 0x8000000000ull;

// CellFiberPpu (896 bytes, 128-aligned).
namespace fib_off {
constexpr u32 state = 0x000, link = 0x004, sched = 0x008, priority = 0x00C, wake_control = 0x010, control = 0x018,
              context = 0x080, entry = 0x300, on_exit = 0x304, arg = 0x308, on_exit_arg = 0x310, status = 0x318,
              detached = 0x31C, next = 0x320, prev = 0x324, self = 0x328, flag = 0x330, flag_value = 0x334,
              debugger = 0x338;
}
// The fiber state word.
enum : u32 {
    kFiberReady = 0x1,
    kFiberYielded = 0x2,
    kFiberRunning = 0x4,
    kFiberWaitSignal = 0x8,
    kFiberSignal = 0x10,
    kFiberWaitFlag = 0x20,
    kFiberExited = 0x40,
    kFiberWaitJoin = 0x80,
    kFiberJoin = 0x100,
};

// The frames libfiber's functions reserve (their r1 relative to their caller's).
namespace frame {
constexpr u32 execute = 928, enter = 176, switch_ = 160, return_to_thread = 128, dispatch = 144, run_fibers = 176,
              yield = 128, wait = 128, exit = 128, wait_flag = 144, join = 240, worker = 256, worker_join = 112,
              fiber_entry = 128, entry_area = 112, reschedule = 176;
// fn executeFiber's frame: its pseudo-context, and where this module keeps the outputs a resume writes.
constexpr u32 pseudo = 192, execute_cause = 840, execute_from = 848;
} // namespace frame

// ------------------------------------------------------------------------------------------------ guest helpers

u8* gb(PPUContext& ctx) { return guest_base(ctx); }
void lwsync() { std::atomic_thread_fence(std::memory_order_acq_rel); }
void hwsync() { std::atomic_thread_fence(std::memory_order_seq_cst); }

// lwarx/ldarx and stwcx./stdcx. (as cellSync2's and cellSpursJq's).
class Reserve {
public:
    u64 ldarx(u8* base, u32 addr)
    {
        addr_ = addr;
        version_ = ps_res_acquire(base, addr, line_);
        return ps_ld64(line_ + (addr & (PS_RES_LINE_SIZE - 1)));
    }
    u32 lwarx(u8* base, u32 addr)
    {
        addr_ = addr;
        version_ = ps_res_acquire(base, addr, line_);
        return ps_ld32(line_ + (addr & (PS_RES_LINE_SIZE - 1)));
    }
    bool stdcx(u8* base, u64 value) const { return ps_res_store_conditional(base, addr_, 8, value, version_, line_); }
    bool stwcx(u8* base, u32 value) const { return ps_res_store_conditional(base, addr_, 4, value, version_, line_); }

private:
    alignas(16) u8 line_[PS_RES_LINE_SIZE];
    u64 version_ = 0;
    u32 addr_ = 0;
};

// mftb, as libfiber reads it: again while the low word is 0.
u64 timebase()
{
    u64 t;
    do t = ps_ppu_timebase();
    while (static_cast<u32>(t) == 0);
    return t;
}

bool mapped(PPUContext& ctx, u32 addr, u64 size, std::string_view fn, std::string_view what)
{
    if (guest_range(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, gb(ctx),
        std::format("{}: the {} at {:#x} ({:#x} bytes) is not guest memory; CELL_FIBER_ERROR_INVAL", fn, what, addr, size));
    return false;
}

// libfiber's assertion: its message, then the process ends.
[[noreturn]] void fiber_assert(std::string_view file, u32 line, std::string_view function,
    std::string_view library = "libfiber")
{
    PS_FATAL(kChan, "PS3 SDK INTERNAL ASSERTION FAILURE: {}, version {:x}, file {}, line {}, function \"{}\"", library,
        kLibraryVersion, file, line, function);
}
#define FIBER_CHECK(cond, file, line, function)                                                                         \
    do {                                                                                                               \
        if (!(cond)) fiber_assert(file, line, function);                                                               \
    } while (0)

s32 sc(PPUContext& ctx, u32 number, std::initializer_list<u64> args)
{
    return static_cast<s32>(lv2_syscall(ctx, number, args));
}

// _sys_strncpy (NUL padded, as liblv2's), of at most `n` bytes from guest or host text.
void put_name(PPUContext& ctx, u32 dst, u32 src, u32 n)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* b = gb(ctx);
    const auto len = guest_strnlen(t, src, n);
    if (!len || !guest_range(t, dst, n)) {
        log_call_site(ctx, b, std::format("cellFiber: the name at {:#x} is not guest memory; nothing copied", src));
        return;
    }
    std::memmove(b + dst, b + src, *len);
    std::memset(b + dst + *len, 0, n - *len);
}
void put_text(u8* b, u32 dst, std::string_view text, u32 n)
{
    const usize len = std::min<usize>(text.size(), n);
    std::memcpy(b + dst, text.data(), len);
    std::memset(b + dst + len, 0, n - len);
}

// ------------------------------------------------------------------------------------------- process state

// The internal functions a switch goes on at, and the ones libfiber hands out by address.
enum Stub : u32 {
    kStart,
    kEntryReturned,
    kFiberEntry,
    kFiberReturned,
    kReschedule,
    kOnExitJoinFiber,
    kOnExitJoinThread,
    kResumeThread,
    kResumeRunFibers,
    kResumeWorker,
    kResumeEnterScheduler,
    kResumeYield,
    kResumeWaitSignal,
    kResumeJoinFiber,
    kResumeWorkerJoinFiber,
    kResumeWaitFlag,
    kResumeExit,
    kResumeSwitch,
    kResumeReturnToThread,
    kStubCount,
};
struct StubName {
    u32 nid;
    std::string_view name;
};
constexpr StubName kStubs[kStubCount] = {
    {0x6D4EFCB9, "_pspocket_cellFiberPpuContextStart"},
    {0xA6B642C9, "_pspocket_cellFiberPpuContextEntryReturned"},
    {0xB7C31F9C, "_pspocket_cellFiberPpuFiberEntry"},
    {0xA04AC02F, "_pspocket_cellFiberPpuFiberReturned"},
    {0x4D8BBEDA, "_pspocket_cellFiberPpuReschedule"},
    {0xE21FFD2A, "_pspocket_cellFiberPpuOnExitJoinFiber"},
    {0x5A0BDFCC, "_pspocket_cellFiberPpuOnExitJoinThread"},
    {0xD0D56F4E, "_pspocket_cellFiberPpuResumeThread"},
    {0x32D33CC0, "_pspocket_cellFiberPpuResumeRunFibers"},
    {0x18C6A704, "_pspocket_cellFiberPpuResumeWorker"},
    {0x0BAC1E49, "_pspocket_cellFiberPpuResumeEnterScheduler"},
    {0xA403DFE4, "_pspocket_cellFiberPpuResumeYield"},
    {0x0887393E, "_pspocket_cellFiberPpuResumeWaitSignal"},
    {0x705886B9, "_pspocket_cellFiberPpuResumeJoinFiber"},
    {0x5B96C141, "_pspocket_cellFiberPpuResumeWorkerJoinFiber"},
    {0x32A86963, "_pspocket_cellFiberPpuResumeWaitFlag"},
    {0x7498D0CF, "_pspocket_cellFiberPpuResumeExit"},
    {0xE9A2B302, "_pspocket_cellFiberPpuResumeSwitch"},
    {0x0DB9AD81, "_pspocket_cellFiberPpuResumeReturnToThread"},
};

struct FiberGlobals {
    // libfiber's TLS offset variable (r13-relative), -1 until _cellFiberPpuInitialize.
    std::atomic<s32> tls_offset{-1};
    std::mutex mutex;
    std::atomic<bool> linked{false};
    u32 descriptors[kStubCount] = {};
    u32 code[kStubCount] = {};
    u32 registry = 0; // the debugger support's handle table (libfiber's data): on first use
    u32 libprof = 0;  // "cellLibprof"
};

FiberGlobals& globals()
{
    FiberGlobals& g = HleProcess::current()->state<FiberGlobals>();
    if (!g.linked.load(std::memory_order_acquire)) {
        std::lock_guard lock(g.mutex);
        if (!g.linked.load(std::memory_order_relaxed)) {
            HleProcess* p = HleProcess::current();
            u8* b = p->kernel().memory().base();
            for (u32 i = 0; i < kStubCount; i++) {
                g.descriptors[i] = p->link_function(kModule, kStubs[i].nid, kStubs[i].name);
                if (!g.descriptors[i]) PS_FATAL(kChan, "no descriptor for {}", kStubs[i].name);
                g.code[i] = ps_read32(b, g.descriptors[i]);
            }
            g.linked.store(true, std::memory_order_release);
        }
    }
    return g;
}

// The calling thread's TLS block, or 0 when libfiber was not initialized or it would be at 0 (libfiber's checks:
// `plus` is 0 for the context functions, 0x20 for the fiber ones, which test the block's fiber word).
u32 tls_block(PPUContext& ctx, FiberGlobals& g, u32 plus = 0)
{
    const s32 offset = g.tls_offset.load(std::memory_order_relaxed);
    if (offset == -1) return 0;
    const u32 at = static_cast<u32>(ctx.gpr[13]) + static_cast<u32>(offset);
    return at + plus == 0 ? 0 : at;
}

// ----------------------------------------------------------------------------------------------- the switch

// Writes the registers of the caller of an HLE call into a context's save area as libfiber's setjmp lays them out,
// with r1 the frame libfiber's function would be in and LR where the context goes on (a resume stub).
void save_registers(u8* b, const PPUContext& ctx, u32 context, u64 r1, u32 resume)
{
    const u32 s = context + ctx_off::save;
    ps_write64(b, s + save_off::r1, r1);
    ps_write64(b, s + save_off::r2, ctx.gpr[2]);
    for (u32 i = 14; i <= 31; i++) ps_write64(b, s + save_off::gpr + 8 * (i - 14), ctx.gpr[i]);
    ps_write64(b, s + save_off::lr, resume);
    u32 cr = 0;
    for (u32 i = 0; i < 32; i++) cr |= u32{ctx.cr[i] & 1u} << (31 - i);
    ps_write64(b, s + save_off::cr, cr);
    for (u32 i = 14; i <= 31; i++) ps_write64(b, s + save_off::fpr + 8 * (i - 14), std::bit_cast<u64>(ctx.fpr[i]));
    ps_write64(b, s + save_off::vrsave, ctx.vrsave);
    for (u32 i = 20; i <= 31; i++)
        for (u32 k = 0; k < 16; k++) ps_write8(b, s + save_off::vr + 16 * (i - 20) + k, ctx.vr[i].u8[15 - k]);
}

// libfiber's longjmp: the save area's registers back (all eight CR fields, as its mtocrf).
void load_registers(u8* b, PPUContext& ctx, u32 context)
{
    const u32 s = context + ctx_off::save;
    ctx.gpr[1] = ps_read64(b, s + save_off::r1);
    ctx.gpr[2] = ps_read64(b, s + save_off::r2);
    for (u32 i = 14; i <= 31; i++) ctx.gpr[i] = ps_read64(b, s + save_off::gpr + 8 * (i - 14));
    ctx.lr = ps_read64(b, s + save_off::lr);
    const u32 cr = static_cast<u32>(ps_read64(b, s + save_off::cr));
    for (u32 i = 0; i < 32; i++) ctx.cr[i] = static_cast<u8>((cr >> (31 - i)) & 1);
    for (u32 i = 14; i <= 31; i++) ctx.fpr[i] = std::bit_cast<double>(ps_read64(b, s + save_off::fpr + 8 * (i - 14)));
    ctx.vrsave = static_cast<u32>(ps_read64(b, s + save_off::vrsave));
    for (u32 i = 20; i <= 31; i++)
        for (u32 k = 0; k < 16; k++) ctx.vr[i].u8[15 - k] = ps_read8(b, s + save_off::vr + 16 * (i - 20) + k);
}

// Goes back to the thread's innermost guest entry, which goes on at LR with the registers as they are now (D-161,
// D-213). Nothing on the host stack between here and there holds anything to release.
[[noreturn]] void transfer(PPUContext& ctx)
{
    ctx.cia = lv2::kNonLocalReturnCia;
    thread_of(ctx).kernel.syscall(ctx); // _longjmp; NonLocalJump when no guest entry is on the thread
    PS_FATAL(kChan, "a context switch did not leave the thread");
}

// libfiber's switch to `target` (its runContext): a context that never ran starts its entry(arg, value) on its stack
// (libfiber's frame of 112 bytes below the top, both back chains 0), any other goes on where it stopped.
[[noreturn]] void run_context(PPUContext& ctx, u8* b, FiberGlobals& g, u32 value, u32 target)
{
    if (ps_read32(b, target + ctx_off::state) == kInitialized) {
        const u32 entry = ps_read32(b, target + ctx_off::entry), top = ps_read32(b, target + ctx_off::top);
        const u64 arg = ps_read64(b, target + ctx_off::arg);
        ps_write32(b, target + ctx_off::state, kRunning);
        ps_write64(b, top, 0);
        ps_write64(b, top - frame::entry_area, 0);
        ctx.gpr[1] = top - frame::entry_area;
        ctx.gpr[3] = arg;
        ctx.gpr[4] = value;
        ctx.gpr[5] = value;
        ctx.gpr[6] = top;
        ctx.gpr[7] = entry;
        ctx.gpr[9] = 0;
        ctx.lr = g.code[kStart];
        transfer(ctx);
    }
    ps_write32(b, target + ctx_off::state, kRunning);
    load_registers(b, ctx, target);
    ctx.gpr[3] = 1; // the setjmp's second return
    transfer(ctx);
}

// Calls a guest function (a scheduler function, an on-exit callback) on the calling thread with r1 = `sp`, without a
// guest entry of its own: a non-local jump inside it leaves this call as libfiber's longjmp would leave its caller.
u64 call_at(PPUContext& ctx, u8* b, u32 opd, u64 sp, std::initializer_list<u64> args)
{
    const u64 r1 = ctx.gpr[1], r2 = ctx.gpr[2], lr = ctx.lr;
    const u32 cia = ctx.cia;
    const u32 code = ps_read32(b, opd);
    ctx.gpr[1] = sp;
    ctx.gpr[2] = ps_read32(b, opd + 4);
    u32 i = 3;
    for (const u64 a : args) ctx.gpr[i++] = a;
    ctx.gpr[12] = code;
    ctx.lr = lv2::kThreadReturnAddress;
    ps_ppu_call_indirect(&ctx, b, code);
    const u64 result = ctx.gpr[3];
    ctx.gpr[1] = r1;
    ctx.gpr[2] = r2;
    ctx.lr = lr;
    ctx.cia = cia;
    return result;
}

// Returns from a resume stub to the caller of the function that stopped: its frame at `caller`, its return address in
// its LR slot (written when it stopped).
void return_to_caller(PPUContext& ctx, u8* b, u64 caller, s32 result)
{
    ctx.gpr[1] = caller;
    ctx.lr = ps_read64(b, static_cast<u32>(caller) + 16);
    ctx.gpr[3] = static_cast<u64>(static_cast<s64>(result));
}

// -------------------------------------------------------------------------------- the debugger support's table

// libfiber's handle table for debuggers (a free list in its data: 8192 slots; the head word 0xE43A4B26 until the
// first handle, 0xE644F5A8 at the end). Here a guest block of the same shape, made at its first use.
constexpr u32 kRegistryFresh = 0xE43A4B26, kRegistryEnd = 0xE644F5A8, kRegistrySlots = 8192;

u32 registry(PPUContext& ctx, FiberGlobals& g)
{
    std::lock_guard lock(g.mutex);
    if (!g.registry) {
        const u32 size = 12 + 4 * kRegistrySlots;
        const u32 at = HleProcess::current()->main_memory().alloc(size, 0x80);
        if (!at) PS_FATAL(kChan, "no guest memory for libfiber's debugger handles ({:#x} bytes)", size);
        std::memset(gb(ctx) + at, 0, size);
        ps_write32(gb(ctx), at, kRegistryFresh);
        g.registry = at;
    }
    return g.registry;
}

// libfiber's debugger registration of `object` as `type` (3 context, 5 fiber, 7 scheduler): a slot holding
// object | type, its address in *out.
s32 register_object(PPUContext& ctx, FiberGlobals& g, u32 object, u32 type, u32 out)
{
    if (!object || !out) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (object & 0xF) return err(CELL_FIBER_ERROR_ALIGN);
    if (type - 1 > 7) return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 table = registry(ctx, g), end = table + 12 + 4 * kRegistrySlots;
    u32 slot = 0;
    for (Reserve r;;) {
        const u32 head = r.lwarx(b, table);
        slot = head == kRegistryFresh ? table + 12 : head;
        if (slot == kRegistryEnd) return err(CELL_FIBER_ERROR_NOMEM);
        u32 next = ps_read32(b, slot);
        if (!next) next = slot + 4 < end ? slot + 4 : kRegistryEnd;
        lwsync();
        if (r.stwcx(b, next)) break;
    }
    ps_write32(b, slot, object | type);
    ps_write32(b, out, slot);
    return lv2::CELL_OK;
}

s32 unregister_object(PPUContext& ctx, FiberGlobals& g, u32 object, u32 type, u32 handle)
{
    if (!object || !handle) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (object & 0xF) return err(CELL_FIBER_ERROR_ALIGN);
    u8* b = gb(ctx);
    if (type - 1 > 7 || (ps_read32(b, handle) & ~0xFu) != object || (ps_read32(b, handle) & 0xF) != type)
        return err(CELL_FIBER_ERROR_INVAL);
    const u32 table = registry(ctx, g);
    for (Reserve r;;) {
        ps_write32(b, handle, r.lwarx(b, table));
        lwsync();
        if (r.stwcx(b, handle)) break;
    }
    return lv2::CELL_OK;
}

// --------------------------------------------------------------------------------- libfiber's no-ordering mutex

// A word (locked byte, spinners, waiters, wake-ups) and an lv2 semaphore (libfiber's noOrderingLock.c).
s32 mutex_init(PPUContext& ctx, u32 m)
{
    if (!m) return err(CELL_FIBER_ERROR_NULL_POINTER);
    u8* b = gb(ctx);
    ps_write32(b, m, 0);
    // sys_semaphore_attribute_t: SYS_SYNC_PRIORITY, not process-shared, "_fibRt".
    StackVar attr(ctx, b, 0x20);
    ps_write32(b, attr.addr() + 0, 2);
    ps_write32(b, attr.addr() + 4, 0x200);
    put_text(b, attr.addr() + 0x18, "_fibRt", 8);
    return sc(ctx, 90, {m + 4, attr.addr(), 0, 1});
}

s32 mutex_destroy(PPUContext& ctx, u32 m)
{
    if (!m) return err(CELL_FIBER_ERROR_NULL_POINTER);
    u8* b = gb(ctx);
    if (ps_read32(b, m)) return err(CELL_FIBER_ERROR_BUSY);
    return sc(ctx, 91, {ps_read32(b, m + 4)});
}

// _cellNoOrderingMutexLock: takes the lock, spinning 80 timebase ticks, then sleeping on the semaphore.
s32 mutex_lock(PPUContext& ctx, u32 m)
{
    if (!m) return err(CELL_FIBER_ERROR_NULL_POINTER);
    u8* b = gb(ctx);
    u32 arrival = 1, woken = 0; // r5: counts itself a waiter; r3: consumes a wake-up
    for (;;) {
        const u64 start = timebase();
        u32 counted = 0; // r4: counted among the spinners
        for (;;) {
            const auto spinning = [&] { return timebase() - start <= 79 ? 1u : 0u; };
            u32 spin = spinning(); // r6
            bool retime = false;
            for (;;) {
                Reserve r;
                const u32 w = r.lwarx(b, m);
                const u32 free = (w >> 24) == 0 ? 1u : 0u; // r7
                u32 n = (w & 0xFFFFFF) | 0x01000000;
                n = (n & ~0xFF00u) | ((((n >> 8) + arrival - free) & 0xFF) << 8);
                n = (n & ~0xFF0000u) | ((((n >> 16) + (((free ^ 1) & spin) - counted)) & 0xFF) << 16);
                n = (n & ~0xFFu) | ((n - woken) & 0xFF);
                if ((arrival | free) == 0 && counted == spin && woken == 0 && counted != 0) {
                    retime = true; // spinning on: nothing to change
                    break;
                }
                if (!r.stwcx(b, n)) {
                    retime = true;
                    break;
                }
                if (free) {
                    lwsync();
                    return lv2::CELL_OK;
                }
                if (spin) {
                    counted = spin;
                    arrival = 0;
                    woken = 0;
                    spin = spinning();
                    continue;
                }
                // Spun out: sleep until an unlock posts the semaphore, then try again as a woken waiter.
                if (sc(ctx, 92, {ps_read32(b, m + 4), 0}) != lv2::CELL_OK)
                    fiber_assert("noOrderingLock.c", 107, "_cellNoOrderingMutexLock");
                woken = 1;
                arrival = 0;
                break;
            }
            if (!retime) break; // back to a new spin window
        }
    }
}

// _cellNoOrderingMutexUnlock: clears the locked byte; with waiters and no spinner or wake-up pending, posts one.
s32 mutex_unlock(PPUContext& ctx, u32 m)
{
    if (!m) return err(CELL_FIBER_ERROR_NULL_POINTER);
    u8* b = gb(ctx);
    lwsync();
    u32 post = 0;
    for (Reserve r;;) {
        const u32 w = r.lwarx(b, m);
        if ((w >> 24) == 0) return err(CELL_FIBER_ERROR_STAT);
        const u32 rest = w & 0xFFFFFF;
        const u32 quiet = (rest & 0x00FF00FF) == 0 ? 1u : 0u;
        post = ((rest >> 8) & 0xFF) ? quiet : 0;
        if (r.stwcx(b, (rest & ~0xFFu) | ((rest + post) & 0xFF))) break;
    }
    if (post && sc(ctx, 94, {ps_read32(b, m + 4), 1}) != lv2::CELL_OK)
        fiber_assert("noOrderingLock.c", 148, "_cellNoOrderingMutexUnlock");
    return lv2::CELL_OK;
}

void lock_or_assert(PPUContext& ctx, u32 m, std::string_view file, u32 line, std::string_view fn)
{
    if (mutex_lock(ctx, m) != lv2::CELL_OK) fiber_assert(file, line, fn);
}
void unlock_or_assert(PPUContext& ctx, u32 m, std::string_view file, u32 line, std::string_view fn)
{
    if (mutex_unlock(ctx, m) != lv2::CELL_OK) fiber_assert(file, line, fn);
}

// ------------------------------------------------------------------------------------------------- traces

// The scheduler trace's record (16 bytes from buffer + 0x80): event, 2, 0, 0, timebase low word, fiber, thread.
void trace_put(PPUContext& ctx, u32 buffer, u32 fiber, u8 event, u16 flags)
{
    u8* b = gb(ctx);
    const u64 thread = lv2::sys_ppu_thread_get_id(thread_of(ctx));
    const u32 index = ps_read32(b, buffer);
    const u64 now = timebase();
    u32 next = index + 1;
    if (ps_read32(b, buffer + 4) == next) {
        if (!(flags & 1)) return; // full, no wrapping
        next = 0;
    }
    const u32 rec = buffer + 0x80 + 16 * index;
    ps_write32(b, rec + 8, fiber);
    ps_write32(b, rec + 4, static_cast<u32>(now));
    ps_write8(b, rec + 2, 0);
    ps_write8(b, rec + 1, 2);
    ps_write8(b, rec + 0, event);
    ps_write32(b, rec + 12, static_cast<u32>(thread));
    ps_write8(b, rec + 3, 0);
    ps_write32(b, buffer, next);
}

void trace_if_running(PPUContext& ctx, u8* b, u32 sched, u32 fiber, u8 event)
{
    if (ps_read16(b, sched + sch_off::trace_mode) == 2)
        trace_put(ctx, ps_read32(b, sched + sch_off::trace), fiber, event, ps_read16(b, sched + sch_off::trace_flags));
}

// _cellFiberPpuNameTracePut: a fiber's name into the name trace (cellLibprof's), when one is running.
s32 name_trace_put(PPUContext& ctx, u32 sched, u32 fiber)
{
    u8* b = gb(ctx);
    const u32 rec = ps_read32(b, sched + sch_off::name_trace);
    if (!rec) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (!ps_read32(b, rec + 0x10) || ps_read32(b, rec) == ps_read32(b, rec + 8)) return lv2::CELL_OK;
    u32 offset = 0;
    if (ps_read32(b, rec + 0xC) & 1) {
        for (Reserve r;;) {
            u64 v = r.ldarx(b, rec);
            offset = static_cast<u32>(v >> 32);
            const u32 limit = ps_read32(b, rec + 8);
            u32 hi = offset + 0x40, lo = static_cast<u32>(v);
            if (limit == hi) hi = 0;
            if (lo == hi) lo += 0x40;
            v = u64{hi} << 32 | lo;
            if (r.stdcx(b, v)) break;
        }
    } else {
        for (Reserve r;;) {
            offset = r.lwarx(b, rec);
            if (r.stwcx(b, offset + 0x40)) break;
        }
    }
    const u32 entry = ps_read32(b, sched + sch_off::name_trace) + 0x80 + offset;
    ps_write8(b, entry + 0, 0x85);
    ps_write8(b, entry + 1, 0x0E);
    ps_write8(b, entry + 3, 0);
    ps_write8(b, entry + 2, 0);
    ps_write32(b, entry + 4, static_cast<u32>(timebase()));
    put_name(ctx, entry + 0xC, fiber + fib_off::context + ctx_off::name, 0x20);
    ps_write32(b, entry + 8, fiber);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------ the run queues

// Puts a fiber at the end of its priority's run queue (the scheduler's mutex held).
void enqueue(u8* b, u32 sched, u32 fiber)
{
    const u32 p = ps_read8(b, fiber + fib_off::priority);
    ps_write32(b, fiber + fib_off::link, 0);
    const u32 tail = ps_read32(b, sched + sch_off::tail + 4 * p);
    if (tail) ps_write32(b, tail + fib_off::link, fiber);
    else ps_write32(b, sched + sch_off::head + 4 * p, fiber);
    ps_write32(b, sched + sch_off::tail + 4 * p, fiber);
    ps_write8(b, sched + sch_off::nonempty + p, 0xFF);
}

// Takes a fiber out of the scheduler's ring of all fibers.
void unlink_fiber(u8* b, u32 fiber)
{
    const u32 next = ps_read32(b, fiber + fib_off::next), prev = ps_read32(b, fiber + fib_off::prev);
    ps_write32(b, next + 4, prev);
    ps_write32(b, fiber + fib_off::prev, 0);
    ps_write32(b, prev, next);
    ps_write32(b, fiber + fib_off::next, 0);
}

// Pushes a fiber that becomes runnable onto its priority's lock-free list (any thread), and marks the priority.
void push_pending(u8* b, u32 sched, u32 fiber)
{
    const u32 p = ps_read8(b, fiber + fib_off::priority);
    const u32 list = sched + sch_off::pending + 4 * p;
    for (Reserve r;;) {
        ps_write32(b, fiber + fib_off::link, r.lwarx(b, list));
        lwsync();
        if (r.stwcx(b, fiber)) break;
    }
    lwsync();
    ps_write8(b, sched + sch_off::nonempty + p, 0xFF);
}

// A signal (reason 3) or a join wake-up (5) for a fiber: runnable again when it waits for it, else kept pending.
// STAT for a fiber that is finalized or has exited.
s32 signal_fiber(u8* b, u32 fiber, u32 sched, u32 reason)
{
    const u32 pending = reason == 3 ? kFiberSignal : kFiberJoin, waiting = reason == 3 ? kFiberWaitSignal : kFiberWaitJoin;
    lwsync();
    bool woke = false;
    for (Reserve r;;) {
        const u32 w = r.lwarx(b, fiber);
        if (w == 0 || (w & kFiberExited)) return err(CELL_FIBER_ERROR_STAT);
        woke = (w & waiting) != 0;
        if (r.stwcx(b, woke ? ((w & ~waiting) | kFiberReady) : (w | pending))) break;
    }
    if (woke) push_pending(b, sched, fiber);
    return lv2::CELL_OK;
}

s32 check_flags(PPUContext& ctx, u32 sched);
s32 has_runnable(PPUContext& ctx, u32 sched, u32 out);

// --------------------------------------------------------------------------------------- libfiber's scheduler

// libfiber's scheduler function (onSwitch): `reason` says what the fiber that entered it does (0 the first pass of
// RunFibers, 1 yield, 2 wait for a flag (its record in the high word), 3 wait for a signal, 4 exit, 5 wait for a
// join); then the first fiber of the highest priority runs (its context returned), or none: 0, back to the thread.
// `sp` is its frame on the thread's stack, where it calls an exiting fiber's on-exit callback.
u32 reschedule(PPUContext& ctx, FiberGlobals& g, u32 tls, u32 sched, u64 reason, u64 sp)
{
    u8* b = gb(ctx);
    constexpr std::string_view kFile = "fiber_ppu.c", kFn = "onSwitch";
    const u32 m = sched + sch_off::mutex;
    const u32 fiber = ps_read32(b, tls + tls_off::fiber);
    ps_write32(b, tls + tls_off::fiber, 0);
    const u32 why = static_cast<u32>(reason) <= 5 ? static_cast<u32>(reason) : 6;
    switch (why) {
    case 1: // yield: to the end of its queue
        for (Reserve r;;) {
            const u32 w = r.lwarx(b, fiber);
            if (r.stwcx(b, (w & ~kFiberRunning) | kFiberYielded)) break;
        }
        lock_or_assert(ctx, m, kFile, 203, kFn);
        enqueue(b, sched, fiber);
        trace_if_running(ctx, b, sched, fiber, 0x83);
        break;
    case 2: { // wait for a flag: its record goes on the scheduler's list, cellFiberPpuCheckFlags wakes it
        for (Reserve r;;) {
            const u32 w = r.lwarx(b, fiber);
            if (r.stwcx(b, (w & ~(kFiberRunning | kFiberReady)) | kFiberWaitFlag | kFiberYielded)) break;
        }
        const u32 record = static_cast<u32>(reason >> 32);
        for (Reserve r;;) {
            ps_write32(b, record, r.lwarx(b, sched + sch_off::flags));
            if (r.stwcx(b, record)) break;
        }
        lock_or_assert(ctx, m, kFile, 223, kFn);
        trace_if_running(ctx, b, sched, fiber, 0x84);
        break;
    }
    case 3:
    case 5: { // wait for a signal or a join wake-up, unless one came first
        const u32 pending = why == 3 ? kFiberSignal : kFiberJoin, waiting = why == 3 ? kFiberWaitSignal : kFiberWaitJoin;
        bool waits = false;
        for (Reserve r;;) {
            const u32 w = r.lwarx(b, fiber);
            waits = (w & pending) == 0;
            if (r.stwcx(b, waits ? ((w & ~(kFiberRunning | kFiberReady)) | kFiberYielded | waiting) : (w & ~pending))) break;
        }
        lock_or_assert(ctx, m, kFile, 247, kFn);
        if (!waits) enqueue(b, sched, fiber);
        trace_if_running(ctx, b, sched, fiber, 0x84);
        break;
    }
    case 4: { // exit
        lock_or_assert(ctx, m, kFile, 269, kFn);
        ps_write32(b, fiber + fib_off::state, kFiberExited);
        if (ps_read32(b, fiber + fib_off::detached) & 1) {
            unlink_fiber(b, fiber);
            if (const u32 handle = ps_read32(b, fiber + fib_off::debugger))
                if (unregister_object(ctx, g, fiber, 5, handle) != lv2::CELL_OK) fiber_assert(kFile, 279, kFn);
        }
        trace_if_running(ctx, b, sched, fiber, 0x82);
        if (const u32 cb = ps_read32(b, fiber + fib_off::on_exit)) {
            unlock_or_assert(ctx, m, kFile, 287, kFn);
            const u64 arg = ps_read64(b, fiber + fib_off::on_exit_arg);
            const s64 status = static_cast<s32>(ps_read32(b, fiber + fib_off::status));
            call_at(ctx, b, cb, sp, {arg, static_cast<u64>(status)});
            lock_or_assert(ctx, m, kFile, 301, kFn);
        }
        break;
    }
    default: // 0, and anything above 5
        lock_or_assert(ctx, m, kFile, 301, kFn);
        break;
    }

    // Now and then (autoCheckFlags), the flags first.
    if (ps_read8(b, sched + sch_off::auto_check)) {
        const u64 last = ps_read64(b, sched + sch_off::last_check), interval = ps_read64(b, sched + sch_off::interval);
        const u64 now = timebase();
        if (last == 0 || last + interval <= now) {
            ps_write64(b, sched + sch_off::last_check, now);
            unlock_or_assert(ctx, m, kFile, 317, kFn);
            if (check_flags(ctx, sched) != lv2::CELL_OK) fiber_assert(kFile, 320, kFn);
            lock_or_assert(ctx, m, kFile, 324, kFn);
        }
    }

    for (u32 p = 0; p < 4; p++) {
        if (!ps_read8(b, sched + sch_off::nonempty + p)) continue;
        bool cleared = false;
        for (;;) {
            // What other threads made runnable, into the run queue.
            const u32 list = sched + sch_off::pending + 4 * p;
            if (ps_read32(b, list)) {
                u32 f = 0;
                for (Reserve r;;) {
                    f = r.lwarx(b, list);
                    if (r.stwcx(b, 0)) break;
                }
                lwsync();
                while (f) {
                    const u32 next = ps_read32(b, f + fib_off::link);
                    ps_write32(b, f + fib_off::link, 0);
                    const u32 tail = ps_read32(b, sched + sch_off::tail + 4 * p);
                    if (tail) ps_write32(b, tail + fib_off::link, f);
                    else ps_write32(b, sched + sch_off::head + 4 * p, f);
                    ps_write32(b, sched + sch_off::tail + 4 * p, f);
                    f = next;
                }
            }
            const u32 head = ps_read32(b, sched + sch_off::head + 4 * p);
            if (head) {
                const u32 next = ps_read32(b, head + fib_off::link);
                ps_write32(b, sched + sch_off::head + 4 * p, next);
                if (!next) ps_write32(b, sched + sch_off::tail + 4 * p, 0);
                if (cleared) ps_write8(b, sched + sch_off::nonempty + p, 0xFF);
                ps_write32(b, tls + tls_off::fiber, head);
                u32 state = 0;
                for (Reserve r;;) {
                    state = r.lwarx(b, head) | kFiberRunning;
                    if (r.stwcx(b, state)) break;
                }
                trace_if_running(ctx, b, sched, head, (state & kFiberYielded) ? 0x81 : 0x80);
                unlock_or_assert(ctx, m, kFile, 342, kFn);
                return head + fib_off::context;
            }
            if (!ps_read8(b, sched + sch_off::nonempty + p)) break;
            ps_write8(b, sched + sch_off::nonempty + p, 0);
            hwsync();
            cleared = true;
        }
    }
    if (!ps_read32(b, tls + tls_off::running)) fiber_assert(kFile, 346, kFn);
    ps_write32(b, tls + tls_off::fiber, 0);
    unlock_or_assert(ctx, m, kFile, 351, kFn);
    return 0;
}

// --------------------------------------------------------------------------------- running scheduler functions

void return_to_thread_now(PPUContext& ctx, u8* b, FiberGlobals& g, u32 tls, s32 cause);

// libfiber's callHookAndDispatch, on the thread's stack at `sp`: the scheduler function picks the next context (0:
// the thread's own), which then runs with the context switched from as its entry's second argument.
[[noreturn]] void dispatch(PPUContext& ctx, u8* b, FiberGlobals& g, u32 tls, u32 desc, u64 a0, u64 a1, u64 sp)
{
    const u32 from = ps_read32(b, tls + tls_off::from);
    u32 next = 0;
    if (desc == g.descriptors[kReschedule]) next = reschedule(ctx, g, tls, static_cast<u32>(a0), a1, sp - frame::reschedule);
    else next = static_cast<u32>(call_at(ctx, b, desc, sp, {a0, a1}));
    if (!next) {
        next = ps_read32(b, tls + tls_off::thread);
        ps_write32(b, tls + tls_off::cause, 0);
    }
    const u32 state = ps_read32(b, next + ctx_off::state);
    if (state != kInitialized && state != kSuspended) {
        // Not a context that can run: back to the thread with CELL_FIBER_ERROR_ABORT.
        return_to_thread_now(ctx, b, g, tls, err(CELL_FIBER_ERROR_ABORT));
        fiber_assert("fiber_ppu_context.c", 317, "callHookAndDispatch");
    }
    ps_write32(b, tls + tls_off::current, next);
    run_context(ctx, b, g, from, next);
}

// libfiber's executeFiber on a thread (cellFiberPpuContextRun, cellFiberPpuContextRunScheduler, RunFibers): the
// thread's own pseudo-context in the frame at `frame` stops (going on at `resume`), then `target` runs, or the
// scheduler function `desc` picks what runs. Returns only libfiber's errors.
s32 execute(PPUContext& ctx, FiberGlobals& g, u32 target, u32 desc, u64 a0, u64 a1, u32 cause, u32 from, u64 frame,
    Stub resume)
{
    u8* b = gb(ctx);
    const u32 tls = tls_block(ctx, g);
    if (!tls) return err(CELL_FIBER_ERROR_NOSYSINIT);
    if (ps_read32(b, tls + tls_off::current)) return err(CELL_FIBER_ERROR_PERM);
    if (ps_read32(b, tls + tls_off::thread)) fiber_assert("fiber_ppu_context.c", 433, "executeFiber");
    const u32 f = static_cast<u32>(frame), pseudo = f + frame::pseudo;
    ps_write32(b, pseudo + ctx_off::state, kRunning);
    ps_write32(b, tls + tls_off::current, pseudo);
    ps_write32(b, tls + tls_off::cause, 0);
    ps_write32(b, tls + tls_off::thread, pseudo);
    save_registers(b, ctx, pseudo, frame, g.code[resume]);
    ps_write64(b, f + frame::execute_cause, cause);
    ps_write64(b, f + frame::execute_from, from);
    ps_write32(b, tls + tls_off::from, 0);
    ps_write32(b, pseudo + ctx_off::state, kSuspended);
    ps_write32(b, tls + tls_off::thread_sp, f);
    if (target) {
        ps_write32(b, tls + tls_off::current, target);
        run_context(ctx, b, g, 0, target);
    }
    // The scheduler function's arguments in the frame, then libfiber's callHookAndDispatch on the same stack.
    ps_write64(b, f + 112, a0);
    ps_write64(b, f + 120, a1);
    ps_write32(b, f + 128, desc);
    dispatch(ctx, b, g, tls, desc, a0, a1, frame - frame::dispatch);
}

// The end of executeFiber when the thread goes on: the outputs, then the thread is no longer in a context.
void finish_execute(u8* b, u32 tls, u32 frame, u32 cause, u32 from)
{
    if (from) ps_write32(b, from, ps_read32(b, tls + tls_off::from));
    if (cause) ps_write32(b, cause, ps_read32(b, tls + tls_off::cause));
    ps_write32(b, frame + frame::pseudo + ctx_off::state, kFinalized);
    ps_write32(b, tls + tls_off::current, 0);
    ps_write32(b, tls + tls_off::cause, 0);
    ps_write32(b, tls + tls_off::thread, 0);
    ps_write32(b, tls + tls_off::thread_sp, 0);
}

// cellFiberPpuContextEnterScheduler's checks.
s32 enter_checks(PPUContext& ctx, FiberGlobals& g, u32& tls, u32& current)
{
    u8* b = gb(ctx);
    tls = tls_block(ctx, g);
    if (!tls) return err(CELL_FIBER_ERROR_NOSYSINIT);
    current = ps_read32(b, tls + tls_off::current);
    if (!current || ps_read32(b, current + ctx_off::state) == kSuspended) return err(CELL_FIBER_ERROR_PERM);
    return lv2::CELL_OK;
}

// cellFiberPpuContextEnterScheduler after its checks: the current context stops in the frame at `es` (going on at
// `resume`), and the scheduler function runs on the thread's stack below the thread's frame, through
// callHookAndDispatch (the arguments block 32 bytes below the thread's r1, the call 144 below it).
[[noreturn]] void enter_scheduler(PPUContext& ctx, FiberGlobals& g, u32 tls, u32 current, u32 desc, u64 a0, u64 a1,
    u64 es, Stub resume)
{
    u8* b = gb(ctx);
    save_registers(b, ctx, current, es, g.code[resume]);
    ps_write32(b, tls + tls_off::from, current);
    ps_write32(b, current + ctx_off::state, kSuspended);
    const u32 thread_sp = ps_read32(b, tls + tls_off::thread_sp);
    const u32 block = thread_sp - 32, base = thread_sp - 144;
    ps_write64(b, block, a0);
    ps_write64(b, block + 8, a1);
    ps_write32(b, block + 16, desc);
    ps_write64(b, base, es); // the stack switch's back chain
    dispatch(ctx, b, g, tls, desc, a0, a1, base - frame::dispatch);
}

// cellFiberPpuContextReturnToThread's way when the current context is suspended (a scheduler function calls it):
// straight to the thread, nothing saved.
void return_to_thread_now(PPUContext& ctx, u8* b, FiberGlobals& g, u32 tls, s32 cause)
{
    ps_write32(b, tls + tls_off::cause, static_cast<u32>(cause));
    const u32 thread = ps_read32(b, tls + tls_off::thread);
    if (ps_read32(b, ps_read32(b, tls + tls_off::current) + ctx_off::state) != kSuspended) return;
    ps_write32(b, tls + tls_off::current, thread);
    run_context(ctx, b, g, ps_read32(b, tls + tls_off::from), thread);
}

// --------------------------------------------------------------------------------------------- the contexts

// libfiber's context initialization (also a fiber's context): the stack marker, the fields, the name.
s32 init_context(PPUContext& ctx, FiberGlobals& g, u32 context, u32 entry, u64 arg, u32 stack, u32 size, u32 sdk,
    u32 name, bool host_name, bool debug)
{
    u8* b = gb(ctx);
    u32 handle = 0;
    if (debug) {
        StackVar out(ctx, b, 8);
        if (const s32 r = register_object(ctx, g, context, 3, out.addr()); r != lv2::CELL_OK) return r;
        handle = ps_read32(b, out.addr());
    }
    ps_write32(b, context + ctx_off::state, kInitialized);
    ps_write32(b, context + ctx_off::top, (stack + size - 1) & ~0xFu);
    ps_write32(b, context + ctx_off::stack_size, size);
    ps_write32(b, context + ctx_off::debugger, handle);
    ps_write32(b, context + ctx_off::sdk, sdk);
    ps_write32(b, context + 4, 0);
    ps_write32(b, context + ctx_off::entry, entry);
    ps_write64(b, context + ctx_off::arg, arg);
    ps_write32(b, context + ctx_off::stack, stack);
    ps_write32(b, context + ctx_off::entry_copy, entry);
    ps_write64(b, context + ctx_off::arg_copy, arg);
    for (u32 i = 0; i < 4; i++) ps_write32(b, stack + 4 * i, kStackMarker);
    if (host_name) put_text(b, context + ctx_off::name, kNoName, 31);
    else put_name(ctx, context + ctx_off::name, name, 31);
    ps_write8(b, context + ctx_off::name + 31, 0);
    return lv2::CELL_OK;
}

// Before a context stops: its stack's marker, for contexts of SDK 3.3 and later.
void check_marker(u8* b, u32 context)
{
    if (ps_read32(b, context + ctx_off::sdk) <= 0x32FFFF) return;
    const u32 stack = ps_read32(b, context + ctx_off::stack);
    for (u32 i = 0; i < 4; i++) {
        if (ps_read32(b, stack + 4 * i) != kStackMarker)
            PS_FATAL(kChan, "libfiber: [fiber: {:#x}] stack overflow detected \n memory check marker on stack top is broken",
                context);
    }
}

s32 _cellFiberPpuContextAttributeInitialize(PPUContext& ctx, u32 attr, u32 sdk)
{
    if (!attr) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (attr & 7) return err(CELL_FIBER_ERROR_ALIGN);
    if (!mapped(ctx, attr, 0x80, "_cellFiberPpuContextAttributeInitialize", "attribute")) return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    std::memset(b + attr, 0, 0x80);
    ps_write32(b, attr + 8, sdk);
    ps_write64(b, attr, kContextAttrMagic);
    put_text(b, attr + 0x10, kNoName, 31);
    ps_write8(b, attr + 0x2F, 0);
    return lv2::CELL_OK;
}

s32 cellFiberPpuContextInitialize(PPUContext& ctx, u32 context, u32 entry, u64 arg, u32 stack, u32 size, u32 attr)
{
    if (!context || !entry || !stack) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if ((context & 0xF) || (stack & 0xF) || (attr & 7)) return err(CELL_FIBER_ERROR_ALIGN);
    const char* fn = "cellFiberPpuContextInitialize";
    if (!mapped(ctx, context, 0x280, fn, "context") || !mapped(ctx, stack, 16, fn, "stack"))
        return err(CELL_FIBER_ERROR_INVAL);
    FiberGlobals& g = globals();
    if (!attr) return init_context(ctx, g, context, entry, arg, stack, size, 0, 0, true, false);
    if (!mapped(ctx, attr, 0x80, fn, "attribute")) return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    if (ps_read64(b, attr) != kContextAttrMagic) return err(CELL_FIBER_ERROR_INVAL);
    return init_context(ctx, g, context, entry, arg, stack, size, ps_read32(b, attr + 8), attr + 0x10, false,
        ps_read8(b, attr + 0x30) != 0);
}

s32 cellFiberPpuContextFinalize(PPUContext& ctx, u32 context)
{
    if (!context) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (context & 0xF) return err(CELL_FIBER_ERROR_ALIGN);
    if (!mapped(ctx, context, 0x280, "cellFiberPpuContextFinalize", "context")) return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 state = ps_read32(b, context + ctx_off::state);
    if (state == kRunning) return err(CELL_FIBER_ERROR_BUSY);
    if (state != kSuspended && state != kInitialized) return err(CELL_FIBER_ERROR_STAT);
    const u32 handle = ps_read32(b, context + ctx_off::debugger);
    ps_write32(b, context + ctx_off::state, kFinalized);
    if (handle && unregister_object(ctx, globals(), context, 3, handle) != lv2::CELL_OK)
        fiber_assert("fiber_ppu_context.c", 200, "cellFiberPpuContextFinalize");
    return lv2::CELL_OK;
}

// cellFiberPpuContextRun(context, cause*, fiberFrom*, option): runs a context on the thread until it returns to the
// thread (cellFiberPpuContextReturnToThread, or a scheduler function with nothing to run).
s32 cellFiberPpuContextRun(PPUContext& ctx, u32 context, u32 cause, u32 from, u32 option)
{
    if (!context || !cause) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (context & 0xF) return err(CELL_FIBER_ERROR_ALIGN);
    if (option) return err(CELL_FIBER_ERROR_INVAL);
    if (!mapped(ctx, context, 0x280, "cellFiberPpuContextRun", "context")) return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 state = ps_read32(b, context + ctx_off::state);
    if (state == kRunning) return err(CELL_FIBER_ERROR_BUSY);
    if (state != kSuspended && state != kInitialized) return err(CELL_FIBER_ERROR_STAT);
    FiberGlobals& g = globals();
    const u64 caller = ctx.gpr[1];
    ps_write64(b, static_cast<u32>(caller) + 16, ctx.lr);
    return execute(ctx, g, context, 0, 0, 0, cause, from, caller - frame::execute, kResumeThread);
}

// cellFiberPpuContextRunScheduler(scheduler, arg0, arg1, cause*, fiberFrom*, option)
s32 cellFiberPpuContextRunScheduler(PPUContext& ctx, u32 scheduler, u64 a0, u64 a1, u32 cause, u32 from, u32 option)
{
    if (!scheduler || !cause) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (option) return err(CELL_FIBER_ERROR_INVAL);
    FiberGlobals& g = globals();
    const u64 caller = ctx.gpr[1];
    ps_write64(gb(ctx), static_cast<u32>(caller) + 16, ctx.lr);
    return execute(ctx, g, 0, scheduler, a0, a1, cause, from, caller - frame::execute, kResumeThread);
}

// cellFiberPpuContextEnterScheduler(scheduler, arg0, arg1, fiberFrom*, option)
s32 cellFiberPpuContextEnterScheduler(PPUContext& ctx, u32 scheduler, u64 a0, u64 a1, u32 from, u32 option)
{
    if (!scheduler) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (option) return err(CELL_FIBER_ERROR_INVAL);
    FiberGlobals& g = globals();
    u32 tls = 0, current = 0;
    if (const s32 r = enter_checks(ctx, g, tls, current); r != lv2::CELL_OK) return r;
    u8* b = gb(ctx);
    const u64 caller = ctx.gpr[1], es = caller - frame::enter;
    ps_write64(b, static_cast<u32>(caller) + 16, ctx.lr);
    ps_write64(b, static_cast<u32>(es) + 112, from);
    enter_scheduler(ctx, g, tls, current, scheduler, a0, a1, es, kResumeEnterScheduler);
}

// cellFiberPpuContextSwitch's checks and switch, from the frame at `cs`.
s32 context_switch(PPUContext& ctx, FiberGlobals& g, u32 target, u32 option, u64 cs, Stub resume)
{
    if (!target) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (target & 0xF) return err(CELL_FIBER_ERROR_ALIGN);
    if (option) return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 tls = tls_block(ctx, g);
    if (!tls) return err(CELL_FIBER_ERROR_NOSYSINIT);
    const u32 current = ps_read32(b, tls + tls_off::current);
    if (!current || ps_read32(b, current + ctx_off::state) == kSuspended) return err(CELL_FIBER_ERROR_PERM);
    if (!mapped(ctx, target, 0x280, "cellFiberPpuContextSwitch", "context")) return err(CELL_FIBER_ERROR_INVAL);
    const u32 state = ps_read32(b, target + ctx_off::state);
    if (state == kRunning) return err(CELL_FIBER_ERROR_BUSY);
    if (state != kSuspended && state != kInitialized) return err(CELL_FIBER_ERROR_STAT);
    check_marker(b, current);
    save_registers(b, ctx, current, cs, g.code[resume]);
    lwsync();
    ps_write32(b, current + ctx_off::state, kSuspended);
    ps_write32(b, tls + tls_off::from, current);
    ps_write32(b, tls + tls_off::current, target);
    run_context(ctx, b, g, current, target);
}

// cellFiberPpuContextSwitch(context, fiberFrom*, option)
s32 cellFiberPpuContextSwitch(PPUContext& ctx, u32 target, u32 from, u32 option)
{
    FiberGlobals& g = globals();
    const u64 caller = ctx.gpr[1], cs = caller - frame::switch_;
    // The return address and fiberFrom go in the frame before the checks: their stores are harmless when it fails.
    ps_write64(gb(ctx), static_cast<u32>(caller) + 16, ctx.lr);
    ps_write64(gb(ctx), static_cast<u32>(cs) + 112, from);
    return context_switch(ctx, g, target, option, cs, kResumeSwitch);
}

// cellFiberPpuContextReturnToThread(cause)
s32 cellFiberPpuContextReturnToThread(PPUContext& ctx, s32 cause)
{
    FiberGlobals& g = globals();
    u8* b = gb(ctx);
    const u32 tls = tls_block(ctx, g);
    if (!tls) return err(CELL_FIBER_ERROR_NOSYSINIT);
    const u32 current = ps_read32(b, tls + tls_off::current);
    if (!current) return err(CELL_FIBER_ERROR_PERM);
    ps_write32(b, tls + tls_off::cause, static_cast<u32>(cause));
    if (ps_read32(b, current + ctx_off::state) == kSuspended) {
        const u32 thread = ps_read32(b, tls + tls_off::thread);
        ps_write32(b, tls + tls_off::current, thread);
        run_context(ctx, b, g, ps_read32(b, tls + tls_off::from), thread);
    }
    const u64 caller = ctx.gpr[1], rt = caller - frame::return_to_thread;
    ps_write64(b, static_cast<u32>(caller) + 16, ctx.lr);
    const s32 r = context_switch(ctx, g, ps_read32(b, tls + tls_off::thread), 0, rt - frame::switch_,
        kResumeReturnToThread);
    if (r != lv2::CELL_OK) fiber_assert("fiber_ppu_context.c", 409, "cellFiberPpuContextReturnToThread");
    return lv2::CELL_OK;
}

u32 cellFiberPpuContextSelf(PPUContext& ctx)
{
    const u32 tls = tls_block(ctx, globals());
    return tls ? ps_read32(gb(ctx), tls + tls_off::current) : 0;
}

// libfiber's stack check: the current context's stack base above r1 (inside its own frame) is an overflow.
s32 stack_limit(PPUContext& ctx, u32 tls, u64 sp)
{
    u8* b = gb(ctx);
    const u32 current = ps_read32(b, tls + tls_off::current);
    if (!mapped(ctx, current + ctx_off::stack, 4, "cellFiberPpuContextCheckStackLimit", "current context"))
        return err(CELL_FIBER_ERROR_STAT);
    return ps_read32(b, current + ctx_off::stack) > static_cast<u32>(sp) ? err(CELL_FIBER_ERROR_STAT) : lv2::CELL_OK;
}

s32 cellFiberPpuContextCheckStackLimit(PPUContext& ctx)
{
    const u32 tls = tls_block(ctx, globals());
    if (!tls) return err(CELL_FIBER_ERROR_PERM);
    return stack_limit(ctx, tls, ctx.gpr[1] - 144);
}

// ------------------------------------------------------------------------------------------- resume stubs

// Each finishes the libfiber function that stopped: r1 is the frame the save area kept, the caller's registers are back.
u32 resume_tls(PPUContext& ctx, FiberGlobals& g)
{
    const u32 tls = tls_block(ctx, g);
    if (!tls) PS_FATAL(kChan, "a context went on on a thread without libfiber's TLS");
    return tls;
}

void _pspocket_cellFiberPpuResumeThread(PPUContext& ctx)
{
    FiberGlobals& g = globals();
    u8* b = gb(ctx);
    const u32 tls = resume_tls(ctx, g), f = static_cast<u32>(ctx.gpr[1]);
    finish_execute(b, tls, f, static_cast<u32>(ps_read64(b, f + frame::execute_cause)),
        static_cast<u32>(ps_read64(b, f + frame::execute_from)));
    return_to_caller(ctx, b, f + frame::execute, lv2::CELL_OK);
}

s32 run_fibers(PPUContext& ctx, FiberGlobals& g, u32 sched, u64 rf, Stub resume);
s32 worker_loop(PPUContext& ctx, FiberGlobals& g, u32 control, u32 index, u32 poller, u64 uwc);

// The rest of cellFiberPpuRunFibers after the scheduler went back to the thread: again while fibers are runnable.
// 1: it returned (its frame at `rf` popped, r3 its answer); for the worker control's loop, the loop goes on.
s32 after_run_scheduler(PPUContext& ctx, FiberGlobals& g, u32 tls, u64 rf)
{
    u8* b = gb(ctx);
    const u32 r = static_cast<u32>(rf), sched = ps_read32(b, r + 128);
    ps_write32(b, tls + tls_off::w2c, 0);
    ps_write32(b, tls + tls_off::running, 0);
    for (Reserve res;;) {
        const u32 w = res.lwarx(b, sched + sch_off::threads);
        if (res.stwcx(b, (w & 0xFFFFFF) | (((w >> 24) - 1) << 24))) break;
    }
    if (has_runnable(ctx, sched, r + 112) != lv2::CELL_OK) fiber_assert("fiber_ppu.c", 609, "cellFiberPpuRunFibers");
    if (ps_read8(b, r + 112)) return run_fibers(ctx, g, sched, rf, kResumeRunFibers);
    return lv2::CELL_OK;
}

void _pspocket_cellFiberPpuResumeRunFibers(PPUContext& ctx)
{
    FiberGlobals& g = globals();
    u8* b = gb(ctx);
    const u32 tls = resume_tls(ctx, g), f = static_cast<u32>(ctx.gpr[1]);
    const u64 rf = u64{f} + frame::execute;
    finish_execute(b, tls, f, static_cast<u32>(rf) + 116, static_cast<u32>(rf) + 120);
    const s32 r = after_run_scheduler(ctx, g, tls, rf);
    return_to_caller(ctx, b, rf + frame::run_fibers, r);
}

void _pspocket_cellFiberPpuResumeWorker(PPUContext& ctx)
{
    FiberGlobals& g = globals();
    u8* b = gb(ctx);
    const u32 tls = resume_tls(ctx, g), f = static_cast<u32>(ctx.gpr[1]);
    const u64 rf = u64{f} + frame::execute, uwc = rf + frame::run_fibers;
    finish_execute(b, tls, f, static_cast<u32>(rf) + 116, static_cast<u32>(rf) + 120);
    // RunFibers again while fibers are runnable (each time leaving through a switch), then the worker's loop.
    const u32 sched = ps_read32(b, static_cast<u32>(rf) + 128);
    ps_write32(b, tls + tls_off::w2c, 0);
    ps_write32(b, tls + tls_off::running, 0);
    for (Reserve res;;) {
        const u32 w = res.lwarx(b, sched + sch_off::threads);
        if (res.stwcx(b, (w & 0xFFFFFF) | (((w >> 24) - 1) << 24))) break;
    }
    if (has_runnable(ctx, sched, static_cast<u32>(rf) + 112) != lv2::CELL_OK)
        fiber_assert("fiber_ppu.c", 609, "cellFiberPpuRunFibers");
    if (ps_read8(b, static_cast<u32>(rf) + 112)) {
        if (run_fibers(ctx, g, sched, rf, kResumeWorker) != lv2::CELL_OK)
            fiber_assert("fiber_ppu_util_worker_control.c", 350, "loopRuntime");
    }
    const u32 u = static_cast<u32>(uwc);
    const s32 r = worker_loop(ctx, g, ps_read32(b, u + 120), ps_read32(b, u + 124), 0, uwc);
    return_to_caller(ctx, b, uwc + frame::worker, r);
}

void _pspocket_cellFiberPpuResumeEnterScheduler(PPUContext& ctx)
{
    FiberGlobals& g = globals();
    u8* b = gb(ctx);
    const u32 tls = resume_tls(ctx, g), es = static_cast<u32>(ctx.gpr[1]);
    if (const u32 out = static_cast<u32>(ps_read64(b, es + 112))) ps_write32(b, out, ps_read32(b, tls + tls_off::from));
    return_to_caller(ctx, b, u64{es} + frame::enter, lv2::CELL_OK);
}

// The fiber functions that enter libfiber's scheduler pass their frame's +112 for fiberFrom.
u64 finish_enter(PPUContext& ctx, FiberGlobals& g, u32 frame_size)
{
    u8* b = gb(ctx);
    const u32 tls = resume_tls(ctx, g);
    const u64 outer = ctx.gpr[1] + frame::enter;
    ps_write32(b, static_cast<u32>(outer) + 112, ps_read32(b, tls + tls_off::from));
    return outer + frame_size;
}

void _pspocket_cellFiberPpuResumeYield(PPUContext& ctx)
{
    const u64 caller = finish_enter(ctx, globals(), frame::yield);
    return_to_caller(ctx, gb(ctx), caller, lv2::CELL_OK);
}

void _pspocket_cellFiberPpuResumeWaitSignal(PPUContext& ctx)
{
    const u64 caller = finish_enter(ctx, globals(), frame::wait);
    lwsync();
    return_to_caller(ctx, gb(ctx), caller, lv2::CELL_OK);
}

void _pspocket_cellFiberPpuResumeWaitFlag(PPUContext& ctx)
{
    const u64 caller = finish_enter(ctx, globals(), frame::wait_flag);
    lwsync();
    return_to_caller(ctx, gb(ctx), caller, lv2::CELL_OK);
}

void _pspocket_cellFiberPpuResumeExit(PPUContext& ctx)
{
    const u64 caller = finish_enter(ctx, globals(), frame::exit);
    return_to_caller(ctx, gb(ctx), caller, lv2::CELL_OK);
}

void _pspocket_cellFiberPpuResumeSwitch(PPUContext& ctx)
{
    FiberGlobals& g = globals();
    u8* b = gb(ctx);
    const u32 tls = resume_tls(ctx, g), cs = static_cast<u32>(ctx.gpr[1]);
    if (const u32 from = static_cast<u32>(ps_read64(b, cs + 112))) ps_write32(b, from, ps_read32(b, tls + tls_off::from));
    return_to_caller(ctx, b, u64{cs} + frame::switch_, lv2::CELL_OK);
}

void _pspocket_cellFiberPpuResumeReturnToThread(PPUContext& ctx)
{
    FiberGlobals& g = globals();
    u8* b = gb(ctx);
    const u32 tls = resume_tls(ctx, g);
    const u64 rt = ctx.gpr[1] + frame::switch_;
    ps_write32(b, static_cast<u32>(rt) + 112, ps_read32(b, tls + tls_off::from));
    return_to_caller(ctx, b, rt + frame::return_to_thread, lv2::CELL_OK);
}

// ------------------------------------------------------------------------- a context's start and its end

// A context that never ran: its entry(arg, fiberFrom) on its stack (r3, r4 and r7 = the entry's descriptor set by
// the switch), returning to a zero word in libfiber (here a fatal log).
void _pspocket_cellFiberPpuContextStart(PPUContext& ctx)
{
    FiberGlobals& g = globals();
    u8* b = gb(ctx);
    const u32 desc = static_cast<u32>(ctx.gpr[7]);
    const u32 code = ps_read32(b, desc);
    ctx.gpr[0] = code;
    ctx.ctr = code;
    ctx.gpr[2] = ps_read32(b, desc + 4);
    ctx.lr = g.code[kEntryReturned];
    ps_ppu_call_indirect(&ctx, b, code);
    PS_FATAL(kChan, "the entry {:#x} of a libfiber context returned (an illegal instruction on the console)", desc);
}

void _pspocket_cellFiberPpuContextEntryReturned(PPUContext& ctx)
{
    PS_FATAL(kChan, "a libfiber context's entry returned (an illegal instruction on the console); r1 {:#x}", ctx.gpr[1]);
}

void exit_fiber(PPUContext& ctx, s32 status);

// A fiber's context entry (libfiber's fiber trampoline): its entry(arg) in a 128-byte frame, then cellFiberPpuExit
// with what it returned.
void _pspocket_cellFiberPpuFiberEntry(PPUContext& ctx, u32 fiber)
{
    FiberGlobals& g = globals();
    u8* b = gb(ctx);
    const u64 sp = ctx.gpr[1];
    const u32 f = static_cast<u32>(sp) - frame::fiber_entry;
    ps_write64(b, static_cast<u32>(sp) + 16, ctx.lr);
    ps_write64(b, f, sp);
    ps_write64(b, f + 40, ctx.gpr[2]);
    const u32 entry = ps_read32(b, fiber + fib_off::entry);
    const u32 code = ps_read32(b, entry);
    ctx.gpr[1] = f;
    ctx.gpr[2] = ps_read32(b, entry + 4);
    ctx.gpr[3] = ps_read64(b, fiber + fib_off::arg);
    ctx.ctr = code;
    ctx.lr = g.code[kFiberReturned];
    ps_ppu_call_indirect(&ctx, b, code);
    // It returned without leaving the thread: the same as the return stub.
    ctx.gpr[1] = f + frame::fiber_entry;
    ctx.lr = ps_read64(b, static_cast<u32>(sp) + 16);
    exit_fiber(ctx, static_cast<s32>(ctx.gpr[3]));
}

// A fiber's entry returned where the fiber went on after a switch: r1 its trampoline's frame.
void _pspocket_cellFiberPpuFiberReturned(PPUContext& ctx)
{
    u8* b = gb(ctx);
    ctx.gpr[1] += frame::fiber_entry;
    ctx.lr = ps_read64(b, static_cast<u32>(ctx.gpr[1]) + 16);
    exit_fiber(ctx, static_cast<s32>(ctx.gpr[3]));
}

// ---------------------------------------------------------------------------------- the fiber scheduler

s32 _cellFiberPpuSchedulerAttributeInitialize(PPUContext& ctx, u32 attr, u32 sdk)
{
    if (!attr) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (attr & 7) return err(CELL_FIBER_ERROR_ALIGN);
    if (!mapped(ctx, attr, 0x100, "_cellFiberPpuSchedulerAttributeInitialize", "attribute")) return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    std::memset(b + attr, 0, 0x100);
    ps_write32(b, attr + 8, sdk);
    ps_write64(b, attr, kSchedulerAttrMagic);
    return lv2::CELL_OK;
}

s32 _cellFiberPpuAttributeInitialize(PPUContext& ctx, u32 attr, u32 sdk)
{
    if (!attr) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (attr & 7) return err(CELL_FIBER_ERROR_ALIGN);
    if (!mapped(ctx, attr, 0x100, "_cellFiberPpuAttributeInitialize", "attribute")) return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    std::memset(b + attr, 0, 0x100);
    ps_write32(b, attr + 8, sdk);
    ps_write64(b, attr, kFiberAttrMagic);
    put_text(b, attr + 0x10, kNoName, 31);
    ps_write8(b, attr + 0x2F, 0);
    return lv2::CELL_OK;
}

// libfiber's user traces with cellLibprof (when that module is loaded).
s32 register_user_traces(PPUContext& ctx, FiberGlobals& g, u32 sched)
{
    u8* b = gb(ctx);
    ps_write32(b, sched + sch_off::user_trace1, 0xFFFFFFFF);
    ps_write32(b, sched + sch_off::user_trace0, 0xFFFFFFFF);
    u32 name = 0;
    {
        std::lock_guard lock(g.mutex);
        if (!g.libprof) {
            g.libprof = HleProcess::current()->main_memory().alloc(16, 16);
            if (g.libprof) put_text(b, g.libprof, "cellLibprof", 16);
        }
        name = g.libprof;
    }
    const u32 nid = 0xE0998DBF; // sys_prx_get_module_id_by_name
    const Function* f = pspocket::hle::registry().find("sysPrxForUser", nid);
    if (!f || !name) return lv2::CELL_OK;
    const u64 saved[3] = {ctx.gpr[3], ctx.gpr[4], ctx.gpr[5]};
    ctx.gpr[3] = name;
    ctx.gpr[4] = 0;
    ctx.gpr[5] = 0;
    f->fn(ctx, b);
    const s32 id = static_cast<s32>(ctx.gpr[3]);
    ctx.gpr[3] = saved[0];
    ctx.gpr[4] = saved[1];
    ctx.gpr[5] = saved[2];
    if (static_cast<u32>(id) == kPrxUnknownModule) return lv2::CELL_OK;
    if (id < 0) return id;
    PS_LOG_ERROR(kChan, "cellLibprof is loaded (module {:#x}): libfiber's user traces are not connected to it here", id);
    return lv2::CELL_OK;
}

s32 cellFiberPpuInitializeScheduler(PPUContext& ctx, u32 sched, u32 attr)
{
    if (!sched) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if ((sched & 0x7F) || (attr & 7)) return err(CELL_FIBER_ERROR_ALIGN);
    const char* fn = "cellFiberPpuInitializeScheduler";
    if (!mapped(ctx, sched, 0x200, fn, "scheduler")) return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    if (attr) {
        if (!mapped(ctx, attr, 0x100, fn, "attribute")) return err(CELL_FIBER_ERROR_INVAL);
        if (ps_read64(b, attr) != kSchedulerAttrMagic) return err(CELL_FIBER_ERROR_INVAL);
    }
    FiberGlobals& g = globals();
    const u32 tls = tls_block(ctx, g, tls_off::fiber);
    if (!tls) return err(CELL_FIBER_ERROR_NOSYSINIT);
    std::memset(b + sched, 0, 0x200);
    ps_write8(b, sched + sch_off::initialized, 1);
    ps_write32(b, sched + sch_off::flags, 0);
    ps_write32(b, sched + sch_off::ring_e0 + 4, sched + sch_off::ring_e0);
    ps_write32(b, sched + sch_off::fibers + 4, sched + sch_off::fibers);
    ps_write32(b, sched + sch_off::ring_e0, sched + sch_off::ring_e0);
    ps_write32(b, sched + sch_off::fibers, sched + sch_off::fibers);
    const bool debug = attr && ps_read8(b, attr + 0x11);
    if (attr) {
        const u64 freq = lv2_syscall(ctx, 147, {}); // sys_time_get_timebase_frequency: the doubleword in r3
        const u32 usec = ps_read32(b, attr + 0x14);
        ps_write8(b, sched + sch_off::auto_check, ps_read8(b, attr + 0x10));
        ps_write64(b, sched + sch_off::last_check, 0);
        ps_write64(b, sched + sch_off::interval, (freq * usec) / 1000000);
        if (debug) {
            if (const s32 r = register_object(ctx, g, sched, 7, sched + sch_off::debugger); r != lv2::CELL_OK) return r;
        }
    }
    ps_write32(b, tls + tls_off::w2c, 0);
    constexpr std::string_view kFile = "fiber_ppu.c", kFn = "cellFiberPpuInitializeScheduler";
    if (const s32 r = mutex_init(ctx, sched + sch_off::mutex); r != lv2::CELL_OK) {
        if (debug && unregister_object(ctx, g, sched, 7, ps_read32(b, sched + sch_off::debugger)) != lv2::CELL_OK)
            fiber_assert(kFile, 453, kFn);
        return r;
    }
    if (const s32 r = mutex_init(ctx, sched + sch_off::mutex2); r != lv2::CELL_OK) {
        if (mutex_destroy(ctx, sched + sch_off::mutex) != lv2::CELL_OK) fiber_assert(kFile, 461, kFn);
        if (debug && unregister_object(ctx, g, sched, 7, ps_read32(b, sched + sch_off::debugger)) != lv2::CELL_OK)
            fiber_assert(kFile, 465, kFn);
        return r;
    }
    if (register_user_traces(ctx, g, sched) != lv2::CELL_OK) fiber_assert(kFile, 472, kFn);
    return lv2::CELL_OK;
}

s32 cellFiberPpuFinalizeScheduler(PPUContext& ctx, u32 sched)
{
    if (!sched) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (sched & 0x7F) return err(CELL_FIBER_ERROR_ALIGN);
    if (!mapped(ctx, sched, 0x200, "cellFiberPpuFinalizeScheduler", "scheduler")) return err(CELL_FIBER_ERROR_INVAL);
    FiberGlobals& g = globals();
    if (!tls_block(ctx, g, tls_off::fiber)) return err(CELL_FIBER_ERROR_NOSYSINIT);
    u8* b = gb(ctx);
    if (ps_read8(b, sched + sch_off::initialized) != 1) return err(CELL_FIBER_ERROR_STAT);
    constexpr std::string_view kFile = "fiber_ppu.c", kFn = "cellFiberPpuFinalizeScheduler";
    const u32 m = sched + sch_off::mutex;
    lock_or_assert(ctx, m, kFile, 504, kFn);
    for (Reserve r;;) {
        const u32 w = r.lwarx(b, sched + sch_off::threads);
        if (((w >> 16) & 0xFF) != 1) {
            unlock_or_assert(ctx, m, kFile, 513, kFn);
            return err(CELL_FIBER_ERROR_STAT);
        }
        if ((w >> 24) != 0 || ps_read32(b, sched + sch_off::fibers) != sched + sch_off::fibers) {
            unlock_or_assert(ctx, m, kFile, 521, kFn);
            return err(CELL_FIBER_ERROR_BUSY);
        }
        if (r.stwcx(b, (w & 0xFF000000) | (w & 0xFFFF))) break;
    }
    unlock_or_assert(ctx, m, kFile, 528, kFn);
    // The user traces (never registered here: see register_user_traces).
    if (mutex_destroy(ctx, m) != lv2::CELL_OK) fiber_assert(kFile, 535, kFn);
    if (mutex_destroy(ctx, sched + sch_off::mutex2) != lv2::CELL_OK) fiber_assert(kFile, 537, kFn);
    if (const u32 handle = ps_read32(b, sched + sch_off::debugger))
        if (unregister_object(ctx, g, sched, 7, handle) != lv2::CELL_OK) fiber_assert(kFile, 541, kFn);
    return lv2::CELL_OK;
}

// cellFiberPpuCheckFlags: every fiber waiting for a flag whose flag is as it waits for becomes runnable; the others
// stay on the list.
s32 check_flags(PPUContext& ctx, u32 sched)
{
    if (!sched) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (sched & 0x7F) return err(CELL_FIBER_ERROR_ALIGN);
    if (!mapped(ctx, sched, 0x200, "cellFiberPpuCheckFlags", "scheduler")) return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    if (ps_read8(b, sched + sch_off::initialized) != 1) return err(CELL_FIBER_ERROR_STAT);
    const u32 list = sched + sch_off::flags;
    u32 record = 0;
    for (Reserve r;;) {
        record = r.lwarx(b, list);
        if (r.stwcx(b, 0)) break;
    }
    u32 kept = 0, last = 0;
    while (record) {
        // A waiting record (on the fiber's stack): +0 next, +8 the flag, +0xC the fiber, +0x10 0 to wake on a flag
        // that is set, else on one that is clear.
        const u32 next = ps_read32(b, record);
        // The flag word, as another PPU thread (or an SPU) stores it while no lock is held (D-571).
        const u32 flag = atomic_load_be32(b, ps_read32(b, record + 8));
        const bool wake = ps_read32(b, record + 0x10) == 0 ? flag != 0 : flag == 0;
        if (wake) {
            const u32 fiber = ps_read32(b, record + 0xC);
            for (Reserve r;;) {
                const u32 w = r.lwarx(b, fiber);
                if (r.stwcx(b, (w & ~kFiberWaitFlag) | kFiberReady)) break;
            }
            push_pending(b, sched, fiber);
        } else {
            if (!kept) kept = record;
            if (last) ps_write32(b, last, record);
            last = record;
        }
        record = next;
    }
    if (kept) {
        ps_write32(b, last, 0);
        for (Reserve r;;) {
            ps_write32(b, last, r.lwarx(b, list));
            if (r.stwcx(b, kept)) break;
        }
    }
    return lv2::CELL_OK;
}

s32 cellFiberPpuCheckFlags(PPUContext& ctx, u32 sched) { return check_flags(ctx, sched); }

s32 has_runnable(PPUContext& ctx, u32 sched, u32 out)
{
    if (!sched || !out) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (sched & 0x7F) return err(CELL_FIBER_ERROR_ALIGN);
    if (!mapped(ctx, sched, 0x200, "cellFiberPpuHasRunnableFiber", "scheduler") ||
        !mapped(ctx, out, 1, "cellFiberPpuHasRunnableFiber", "output"))
        return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    if (!ps_read8(b, sched + sch_off::initialized)) return err(CELL_FIBER_ERROR_STAT);
    ps_write8(b, out, ps_read32(b, sched + sch_off::nonempty) != 0 ? 1 : 0);
    return lv2::CELL_OK;
}

s32 cellFiberPpuHasRunnableFiber(PPUContext& ctx, u32 sched, u32 out) { return has_runnable(ctx, sched, out); }

s32 cellFiberPpuGetScheduler(PPUContext& ctx, u32 fiber, u32 out)
{
    if (!fiber || !out) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (fiber & 0x7F) return err(CELL_FIBER_ERROR_ALIGN);
    if (!mapped(ctx, fiber, 0x380, "cellFiberPpuGetScheduler", "fiber") ||
        !mapped(ctx, out, 4, "cellFiberPpuGetScheduler", "output"))
        return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    ps_write32(b, out, ps_read32(b, fiber + fib_off::sched));
    return lv2::CELL_OK;
}

s32 create_fiber(PPUContext& ctx, u32 sched, u32 fiber, u32 entry, u64 arg, u32 priority, u32 stack, u32 size, u32 attr)
{
    if (!sched || !fiber || !entry || !stack) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if ((sched & 0x7F) || (fiber & 0x7F) || (stack & 0xF) || (size & 0xF)) return err(CELL_FIBER_ERROR_ALIGN);
    u8* b = gb(ctx);
    const char* fn = "cellFiberPpuCreateFiber";
    if (priority > 3) return err(CELL_FIBER_ERROR_INVAL);
    if (attr) {
        if (!mapped(ctx, attr, 0x100, fn, "attribute")) return err(CELL_FIBER_ERROR_INVAL);
        if (ps_read64(b, attr) != kFiberAttrMagic) return err(CELL_FIBER_ERROR_INVAL);
    }
    if (!mapped(ctx, sched, 0x200, fn, "scheduler") || !mapped(ctx, fiber, 0x380, fn, "fiber") ||
        !mapped(ctx, stack, 16, fn, "stack"))
        return err(CELL_FIBER_ERROR_INVAL);
    FiberGlobals& g = globals();
    if (!tls_block(ctx, g, tls_off::fiber)) return err(CELL_FIBER_ERROR_NOSYSINIT);
    ps_write32(b, fiber + fib_off::sched, sched);
    ps_write32(b, fiber + fib_off::entry, entry);
    ps_write64(b, fiber + fib_off::arg, arg);
    ps_write8(b, fiber + fib_off::priority, static_cast<u8>(priority));
    ps_write32(b, fiber + fib_off::self, fiber);
    ps_write32(b, fiber + fib_off::wake_control, 0);
    ps_write64(b, fiber + fib_off::control, 0);
    ps_write32(b, fiber + fib_off::state, kFiberReady);
    ps_write32(b, fiber + fib_off::link, 0);
    ps_write32(b, fiber + fib_off::on_exit, 0);
    ps_write32(b, fiber + fib_off::detached, 0);
    ps_write32(b, fiber + fib_off::debugger, 0);
    u32 sdk = 0;
    if (attr) {
        sdk = ps_read32(b, attr + 8);
        if (const u32 cb = ps_read32(b, attr + 0x30)) {
            ps_write32(b, fiber + fib_off::detached, 1);
            ps_write32(b, fiber + fib_off::on_exit, cb);
            ps_write64(b, fiber + fib_off::on_exit_arg, ps_read64(b, attr + 0x38));
        }
        if (sdk > 0x26FFFF) {
            ps_write64(b, fiber + fib_off::control, ps_read64(b, attr + 0x40));
            ps_write32(b, fiber + fib_off::wake_control, ps_read32(b, attr + 0x34));
        }
    }
    if (ps_read32(b, sched + sch_off::debugger)) {
        if (const s32 r = register_object(ctx, g, fiber, 5, fiber + fib_off::debugger); r != lv2::CELL_OK) return r;
    }
    constexpr std::string_view kFile = "fiber_ppu.c", kFn = "cellFiberPpuCreateFiber";
    if (init_context(ctx, g, fiber + fib_off::context, g.descriptors[kFiberEntry], fiber, stack, size, sdk,
            attr ? attr + 0x10 : 0, !attr, false) != lv2::CELL_OK)
        fiber_assert(kFile, 910, kFn);
    const u32 m = sched + sch_off::mutex;
    lock_or_assert(ctx, m, kFile, 914, kFn);
    // At the end of the ring of all fibers.
    const u32 tail = ps_read32(b, sched + sch_off::fibers + 4);
    ps_write32(b, fiber + fib_off::prev, tail);
    ps_write32(b, fiber + fib_off::next, sched + sch_off::fibers);
    ps_write32(b, sched + sch_off::fibers + 4, fiber + fib_off::next);
    ps_write32(b, tail, fiber + fib_off::next);
    name_trace_put(ctx, sched, fiber);
    enqueue(b, sched, fiber);
    unlock_or_assert(ctx, m, kFile, 922, kFn);
    return lv2::CELL_OK;
}

// cellFiberPpuCreateFiber(scheduler, fiber, entry, arg, priority, stack, stackSize, attr)
s32 cellFiberPpuCreateFiber(PPUContext& ctx, u32 sched, u32 fiber, u32 entry, u64 arg, u32 priority, u32 stack, u32 size,
    u32 attr)
{
    return create_fiber(ctx, sched, fiber, entry, arg, priority, stack, size, attr);
}

u32 cellFiberPpuSelf(PPUContext& ctx)
{
    const u32 tls = tls_block(ctx, globals(), tls_off::fiber);
    return tls ? ps_read32(gb(ctx), tls + tls_off::fiber) : 0;
}

// The fiber calling, through its TLS block: 0 with *error set when there is none.
u32 current_fiber(PPUContext& ctx, FiberGlobals& g, u32& tls, s32& error)
{
    tls = tls_block(ctx, g, tls_off::fiber);
    if (!tls) {
        error = err(CELL_FIBER_ERROR_NOSYSINIT);
        return 0;
    }
    const u32 fiber = ps_read32(gb(ctx), tls + tls_off::fiber);
    if (!fiber) error = err(CELL_FIBER_ERROR_PERM);
    return fiber;
}

// libfiber's reschedule: the fiber enters its scheduler from the frame at `es` for `reason`.
[[noreturn]] void enter_fiber_scheduler(PPUContext& ctx, FiberGlobals& g, u32 sched, u64 reason, u64 es, Stub resume)
{
    u32 tls = 0, current = 0;
    if (enter_checks(ctx, g, tls, current) != lv2::CELL_OK) fiber_assert("fiber_ppu.c", 366, "reschedule");
    enter_scheduler(ctx, g, tls, current, g.descriptors[kReschedule], sched, reason, es, resume);
}

s32 cellFiberPpuYield(PPUContext& ctx)
{
    FiberGlobals& g = globals();
    u32 tls = 0;
    s32 error = 0;
    const u32 fiber = current_fiber(ctx, g, tls, error);
    if (!fiber) return error;
    u8* b = gb(ctx);
    const u64 caller = ctx.gpr[1], y = caller - frame::yield;
    ps_write64(b, static_cast<u32>(caller) + 16, ctx.lr);
    enter_fiber_scheduler(ctx, g, ps_read32(b, fiber + fib_off::sched), 1, y - frame::enter, kResumeYield);
}

// libfiber's wait for a signal (3) or a join wake-up (5) from the frame at `w`: none when one came first.
s32 wait_for(PPUContext& ctx, FiberGlobals& g, u32 fiber, u32 sched, u32 reason, u64 w, Stub resume)
{
    u8* b = gb(ctx);
    const u32 bit = reason == 3 ? kFiberSignal : kFiberJoin;
    bool had = false;
    for (Reserve r;;) {
        const u32 v = r.lwarx(b, fiber);
        had = (v & bit) != 0;
        if (r.stwcx(b, had ? v & ~bit : v)) break;
    }
    if (!had) enter_fiber_scheduler(ctx, g, sched, reason, w - frame::enter, resume);
    lwsync();
    return lv2::CELL_OK;
}

s32 cellFiberPpuWaitSignal(PPUContext& ctx)
{
    FiberGlobals& g = globals();
    u32 tls = 0;
    s32 error = 0;
    const u32 fiber = current_fiber(ctx, g, tls, error);
    if (!fiber) return error;
    u8* b = gb(ctx);
    const u64 caller = ctx.gpr[1];
    ps_write64(b, static_cast<u32>(caller) + 16, ctx.lr);
    return wait_for(ctx, g, fiber, ps_read32(b, fiber + fib_off::sched), 3, caller - frame::wait, kResumeWaitSignal);
}

// cellFiberPpuWaitFlag(flag*, value): until the flag is set (value 1) or clear (0); cellFiberPpuCheckFlags looks.
s32 cellFiberPpuWaitFlag(PPUContext& ctx, u32 flag, u32 value)
{
    if (!flag) return err(CELL_FIBER_ERROR_NULL_POINTER);
    FiberGlobals& g = globals();
    u32 tls = 0;
    s32 error = 0;
    const u32 fiber = current_fiber(ctx, g, tls, error);
    if (!fiber) return error;
    if (!mapped(ctx, flag, 4, "cellFiberPpuWaitFlag", "flag")) return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    const u64 caller = ctx.gpr[1], wf = caller - frame::wait_flag;
    const u32 record = static_cast<u32>(wf) + 116;
    const u32 v = value & 0xFF;
    ps_write32(b, record + 0xC, fiber);
    ps_write32(b, record + 0x10, (value ^ 1) & 0xFF);
    ps_write32(b, record + 8, flag);
    const u32 sched = ps_read32(b, fiber + fib_off::sched);
    const u32 now = atomic_load_be32(b, flag); // another thread's store (D-571)
    if (v == 0 ? now != 0 : now == 0) {
        ps_write32(b, fiber + fib_off::flag_value, v);
        ps_write32(b, fiber + fib_off::flag, flag);
        ps_write64(b, static_cast<u32>(caller) + 16, ctx.lr);
        enter_fiber_scheduler(ctx, g, sched, u64{record} << 32 | 2, wf - frame::enter, kResumeWaitFlag);
    }
    lwsync();
    return lv2::CELL_OK;
}

// cellFiberPpuExit's work, from a caller at r1 whose return address is in LR.
s32 exit_current(PPUContext& ctx, s32 status)
{
    FiberGlobals& g = globals();
    u32 tls = 0;
    s32 error = 0;
    const u32 fiber = current_fiber(ctx, g, tls, error);
    if (!fiber) return error;
    u8* b = gb(ctx);
    ps_write32(b, fiber + fib_off::status, static_cast<u32>(status));
    const u64 caller = ctx.gpr[1], e = caller - frame::exit;
    ps_write64(b, static_cast<u32>(caller) + 16, ctx.lr);
    enter_fiber_scheduler(ctx, g, ps_read32(b, fiber + fib_off::sched), 4, e - frame::enter, kResumeExit);
}

void exit_fiber(PPUContext& ctx, s32 status)
{
    const s32 r = exit_current(ctx, status);
    PS_FATAL(kChan, "a fiber's entry returned {:#x} and cellFiberPpuExit refused it ({:#x})", static_cast<u32>(status),
        static_cast<u32>(r));
}

s32 cellFiberPpuExit(PPUContext& ctx, s32 status) { return exit_current(ctx, status); }

s32 cellFiberPpuSendSignal(PPUContext& ctx, u32 fiber, u32 workers)
{
    if (!fiber) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (fiber & 0x7F) return err(CELL_FIBER_ERROR_ALIGN);
    if (!mapped(ctx, fiber, 0x380, "cellFiberPpuSendSignal", "fiber")) return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 sched = ps_read32(b, fiber + fib_off::sched);
    const s32 r = signal_fiber(b, fiber, sched, 3);
    if (r != lv2::CELL_OK) return r;
    lwsync();
    if (workers) ps_write32(b, workers, ps_read8(b, sched + sch_off::threads));
    return lv2::CELL_OK;
}

s32 cellFiberPpuSetPriority(PPUContext& ctx, u32 priority)
{
    FiberGlobals& g = globals();
    u32 tls = 0;
    s32 error = 0;
    const u32 fiber = current_fiber(ctx, g, tls, error);
    if (!fiber) return error;
    if (priority > 3) return err(CELL_FIBER_ERROR_INVAL);
    ps_write8(gb(ctx), fiber + fib_off::priority, static_cast<u8>(priority));
    return lv2::CELL_OK;
}

s32 cellFiberPpuCheckStackLimit(PPUContext& ctx)
{
    FiberGlobals& g = globals();
    u32 tls = 0;
    s32 error = 0;
    if (!current_fiber(ctx, g, tls, error)) return error;
    // cellFiberPpuContextCheckStackLimit in this one's frame of 112 bytes.
    const u32 at = tls_block(ctx, g);
    if (!at) return err(CELL_FIBER_ERROR_PERM);
    return stack_limit(ctx, at, ctx.gpr[1] - 112 - 144);
}

// The rest of a join once the fiber exited: out of the ring, its status, its fiber word cleared.
s32 finish_join(PPUContext& ctx, FiberGlobals& g, u32 target, u32 status, u32 m, bool semaphore, u32 j, bool check)
{
    u8* b = gb(ctx);
    constexpr std::string_view kFile = "fiber_ppu.c", kFn = "cellFiberPpuJoinFiber";
    if (check && ps_read32(b, target + fib_off::state) != kFiberExited) fiber_assert(kFile, 1219, kFn);
    unlink_fiber(b, target);
    ps_write32(b, status, ps_read32(b, target + fib_off::status));
    ps_write32(b, target + fib_off::state, 0);
    unlock_or_assert(ctx, m, kFile, 1226, kFn);
    if (const u32 handle = ps_read32(b, target + fib_off::debugger))
        if (unregister_object(ctx, g, target, 5, handle) != lv2::CELL_OK) fiber_assert(kFile, 1230, kFn);
    if (semaphore && sc(ctx, 91, {ps_read32(b, j + 112)}) != lv2::CELL_OK) fiber_assert(kFile, 1235, kFn);
    return lv2::CELL_OK;
}

// cellFiberPpuJoinFiber from a caller at `caller` (the game's r1, or the worker control's frame).
s32 join_fiber(PPUContext& ctx, u32 target, u32 status, u64 caller, Stub resume)
{
    if (!target || !status) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (target & 0x7F) return err(CELL_FIBER_ERROR_ALIGN);
    FiberGlobals& g = globals();
    const u32 tls = tls_block(ctx, g, tls_off::fiber);
    if (!tls) return err(CELL_FIBER_ERROR_NOSYSINIT);
    u8* b = gb(ctx);
    const u32 self = ps_read32(b, tls + tls_off::fiber);
    if (target == self) return err(CELL_FIBER_ERROR_DEADLK);
    if (!mapped(ctx, target, 0x380, "cellFiberPpuJoinFiber", "fiber") ||
        !mapped(ctx, status, 4, "cellFiberPpuJoinFiber", "status"))
        return err(CELL_FIBER_ERROR_INVAL);
    if (ps_read32(b, target + fib_off::detached) & 1) return err(CELL_FIBER_ERROR_INVAL);
    if (!ps_read32(b, target + fib_off::state)) return err(CELL_FIBER_ERROR_STAT);
    constexpr std::string_view kFile = "fiber_ppu.c", kFn = "cellFiberPpuJoinFiber";
    const u32 sched = ps_read32(b, target + fib_off::sched), m = sched + sch_off::mutex;
    lock_or_assert(ctx, m, kFile, 1158, kFn);
    const u32 j = static_cast<u32>(caller - frame::join);
    bool waiting = false, semaphore = false;
    for (;;) {
        if (ps_read32(b, target + fib_off::on_exit)) {
            unlock_or_assert(ctx, m, kFile, 1162, kFn);
            if (semaphore && sc(ctx, 91, {ps_read32(b, j + 112)}) != lv2::CELL_OK) fiber_assert(kFile, 1165, kFn);
            return err(CELL_FIBER_ERROR_BUSY);
        }
        if (ps_read32(b, target + fib_off::state) == kFiberExited) return finish_join(ctx, g, target, status, m, semaphore, j, false);
        if (self) {
            // A fiber: the target's exit signals it (libfiber's fiberOnExit).
            ps_write64(b, target + fib_off::on_exit_arg, self);
            ps_write32(b, target + fib_off::on_exit, g.descriptors[kOnExitJoinFiber]);
            unlock_or_assert(ctx, m, kFile, 1177, kFn);
            ps_write32(b, j + 152, target);
            ps_write32(b, j + 156, status);
            ps_write64(b, static_cast<u32>(caller) + 16, ctx.lr);
            wait_for(ctx, g, self, sched, 5, u64{j} - frame::wait, resume);
            lock_or_assert(ctx, m, kFile, 1212, kFn);
            return finish_join(ctx, g, target, status, m, semaphore, j, true);
        }
        if (waiting) {
            // A PPU thread: the target's exit posts its semaphore (libfiber's fiberOnExitForPpuThread).
            ps_write32(b, target + fib_off::on_exit, g.descriptors[kOnExitJoinThread]);
            ps_write64(b, target + fib_off::on_exit_arg, j + 112);
            unlock_or_assert(ctx, m, kFile, 1187, kFn);
            if (sc(ctx, 92, {ps_read32(b, j + 112), 0}) != lv2::CELL_OK) fiber_assert(kFile, 1190, kFn);
            lock_or_assert(ctx, m, kFile, 1212, kFn);
            return finish_join(ctx, g, target, status, m, semaphore, j, true);
        }
        unlock_or_assert(ctx, m, kFile, 1196, kFn);
        // sys_semaphore_attribute_t in the frame: SYS_SYNC_PRIORITY, not process-shared, "_fibJo".
        ps_write32(b, j + 120, 2);
        ps_write32(b, j + 124, 0x200);
        ps_write64(b, j + 128, 0);
        ps_write32(b, j + 136, 0);
        put_text(b, j + 144, "_fibJo", 8);
        if (const s32 r = sc(ctx, 90, {j + 112, j + 120, 0, 1}); r != lv2::CELL_OK) return r;
        waiting = true;
        semaphore = true;
        lock_or_assert(ctx, m, kFile, 1212, kFn);
    }
}

s32 cellFiberPpuJoinFiber(PPUContext& ctx, u32 target, u32 status)
{
    return join_fiber(ctx, target, status, ctx.gpr[1], kResumeJoinFiber);
}

// A fiber that waited in cellFiberPpuJoinFiber goes on: fn_34a4's end, then the join's.
s32 resume_join(PPUContext& ctx, u64& j)
{
    FiberGlobals& g = globals();
    u8* b = gb(ctx);
    const u64 w = finish_enter(ctx, g, 0);
    lwsync();
    j = w + frame::wait;
    const u32 jj = static_cast<u32>(j);
    const u32 target = ps_read32(b, jj + 152), status = ps_read32(b, jj + 156);
    const u32 m = ps_read32(b, target + fib_off::sched) + sch_off::mutex;
    lock_or_assert(ctx, m, "fiber_ppu.c", 1212, "cellFiberPpuJoinFiber");
    return finish_join(ctx, g, target, status, m, false, jj, true);
}

void _pspocket_cellFiberPpuResumeJoinFiber(PPUContext& ctx)
{
    u64 j = 0;
    const s32 r = resume_join(ctx, j);
    return_to_caller(ctx, gb(ctx), j + frame::join, r);
}

void _pspocket_cellFiberPpuResumeWorkerJoinFiber(PPUContext& ctx)
{
    u64 j = 0;
    const s32 r = resume_join(ctx, j);
    return_to_caller(ctx, gb(ctx), j + frame::join + frame::worker_join, r);
}

// The join's on-exit functions, called from the scheduler as a fiber exits.
void _pspocket_cellFiberPpuOnExitJoinFiber(PPUContext& ctx, u32 joiner, s32)
{
    u8* b = gb(ctx);
    if (signal_fiber(b, joiner, ps_read32(b, joiner + fib_off::sched), 5) != lv2::CELL_OK)
        fiber_assert("fiber_ppu.c", 1103, "fiberOnExit");
}

void _pspocket_cellFiberPpuOnExitJoinThread(PPUContext& ctx, u32 record, s32)
{
    if (sc(ctx, 94, {ps_read32(gb(ctx), record), 1}) != lv2::CELL_OK)
        fiber_assert("fiber_ppu.c", 1114, "fiberOnExitForPpuThread");
}

// libfiber's scheduler as a guest function (what the scheduler descriptors in its blocks name).
u32 _pspocket_cellFiberPpuReschedule(PPUContext& ctx, u32 sched, u64 reason)
{
    FiberGlobals& g = globals();
    return reschedule(ctx, g, resume_tls(ctx, g), sched, reason, ctx.gpr[1] - frame::reschedule);
}

// cellFiberPpuRunFibers' loop from its thread count on, in the frame at `rf`: leaves through a switch to the first
// runnable fiber (back at `resume` when the scheduler has nothing left), or returns STAT.
s32 run_fibers(PPUContext& ctx, FiberGlobals& g, u32 sched, u64 rf, Stub resume)
{
    u8* b = gb(ctx);
    const u32 tls = tls_block(ctx, g, tls_off::fiber);
    for (Reserve r;;) {
        const u32 w = r.lwarx(b, sched + sch_off::threads);
        if (((w >> 16) & 0xFF) != 1) return err(CELL_FIBER_ERROR_STAT);
        if (r.stwcx(b, (w & 0xFFFFFF) | (((w >> 24) + 1) << 24))) break;
    }
    ps_write32(b, tls + tls_off::running, 1);
    ps_write32(b, tls + tls_off::w24, 0);
    ps_write32(b, tls + tls_off::w2c, 0);
    const u32 r = static_cast<u32>(rf);
    ps_write32(b, r + 128, sched);
    execute(ctx, g, 0, g.descriptors[kReschedule], sched, 0, r + 116, r + 120, rf - frame::execute, resume);
    fiber_assert("fiber_ppu.c", 594, "cellFiberPpuRunFibers");
}

s32 cellFiberPpuRunFibers(PPUContext& ctx, u32 sched)
{
    if (!sched) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (sched & 0x7F) return err(CELL_FIBER_ERROR_ALIGN);
    FiberGlobals& g = globals();
    const u32 tls = tls_block(ctx, g, tls_off::fiber);
    if (!tls) return err(CELL_FIBER_ERROR_NOSYSINIT);
    u8* b = gb(ctx);
    if (ps_read32(b, tls + tls_off::fiber)) return err(CELL_FIBER_ERROR_PERM);
    if (!mapped(ctx, sched, 0x200, "cellFiberPpuRunFibers", "scheduler")) return err(CELL_FIBER_ERROR_INVAL);
    const u64 caller = ctx.gpr[1], rf = caller - frame::run_fibers;
    ps_write64(b, static_cast<u32>(caller) + 16, ctx.lr);
    ps_write64(b, sched + sch_off::last_check, 0);
    ps_write8(b, static_cast<u32>(rf) + 112, 0);
    return run_fibers(ctx, g, sched, rf, kResumeRunFibers);
}

// ------------------------------------------------------------------------------- the utility worker control

s32 worker_wakeup(PPUContext& ctx, u32 control)
{
    if (!control) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (control & 0x7F) return err(CELL_FIBER_ERROR_ALIGN);
    if (!mapped(ctx, control, 0x300, "cellFiberPpuUtilWorkerControlWakeup", "worker control")) return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    u64 old = 0;
    for (Reserve r;;) {
        old = r.ldarx(b, control + uwc_off::word);
        const u64 n = (old & ~(0xFFull << 40) & ~0xFFFFFFFFull) | (1ull << 40);
        if (!(old & kUwcInitialized) && ps_read32(b, control + uwc_off::sdk) > 0x2FFFFF) return err(CELL_FIBER_ERROR_STAT);
        if (r.stdcx(b, n)) break;
    }
    if (static_cast<u32>(old)) {
        const s32 rc = sc(ctx, 87, {ps_read32(b, control + uwc_off::flag), static_cast<u32>(old)});
        if (rc != lv2::CELL_OK && static_cast<u32>(rc) != kLv2Busy)
            fiber_assert("fiber_ppu_util_worker_control.c", 206, "cellFiberPpuUtilWorkerControlWakeup");
    }
    return lv2::CELL_OK;
}

s32 cellFiberPpuUtilWorkerControlWakeup(PPUContext& ctx, u32 control) { return worker_wakeup(ctx, control); }

// The worker's loop (libfiber's loopRuntime) from its outer loop: runs the fibers whenever some are runnable, sleeps
// on its event flag bit otherwise (one worker polling when the polling mode is on), until shut down. `poller` is
// libfiber's r30 at the loop's top. Leaves through a switch when it runs fibers (the resume stub calls it again).
s32 worker_loop(PPUContext& ctx, FiberGlobals& g, u32 control, u32 index, u32 poller, u64 uwc)
{
    u8* b = gb(ctx);
    constexpr std::string_view kFile = "fiber_ppu_util_worker_control.c", kFn = "loopRuntime";
    const u32 u = static_cast<u32>(uwc);
    const u32 mine = index >= 32 ? 0 : 1u << index;
    const u32 bitmap_bit = index >= 32 ? 0 : 0x80000000u >> index;
    const u64 not_mine = ~u64{mine};
    u32 r30 = poller;
    for (;;) {
        u32 spins = 2, r26 = 0, initialized = 0;
        bool sleep = false;
        u64 others = 0;
        for (;;) {
            check_flags(ctx, control);
            if (has_runnable(ctx, control, u + 112) != lv2::CELL_OK) fiber_assert(kFile, 300, kFn);
            for (;;) {
                Reserve r;
                const u64 c = r.ldarx(b, control + uwc_off::word);
                const bool runnable = ps_read8(b, u + 112) != 0;
                const u64 cleared = c & not_mine;
                initialized = static_cast<u32>((c >> 38) & 1);
                r26 = 0;
                sleep = false;
                u64 n = 0;
                if (!runnable && initialized && ((c >> 40) & 0xFF) == 0) {
                    // To sleep: this worker's bit among the sleepers, maybe the polling one.
                    n = (c & ~0xFFFFFFFFull) | (u64{mine} | (cleared & 0xFFFFFFFFull));
                    u32 take = 0;
                    others = 0;
                    sleep = true;
                    if ((n & kUwcPolling) && (((n >> 48) & 0xFF) == 0 || r30 != 0)) {
                        take = 1;
                        r26 = 1;
                    }
                    const u32 pollers = static_cast<u32>(n >> 48) + take - r30;
                    n = (n & ~(0xFFull << 48)) | (u64{pollers & 0xFF} << 48);
                    n &= ~(0xFFull << 40);
                } else {
                    // To run (or leave): every sleeper woken, this one no longer polling.
                    n = (c & ~0xFFFFFFFFull) | (cleared & 0xFFFFFFFFull);
                    const u32 pollers = static_cast<u32>(n >> 48) - r30;
                    n = (n & ~(0xFFull << 48)) | (u64{pollers & 0xFF} << 48);
                    others = n;
                    n &= ~0xFFFFFFFFull;
                    n &= ~(0xFFull << 40);
                }
                if (r.stdcx(b, n)) break;
            }
            if (sleep && --spins != 0 && initialized) {
                r30 = r26; // once more before sleeping
                continue;
            }
            break;
        }
        r30 = initialized;
        const u32 timeout = r26 ? ps_read32(b, control + uwc_off::interval) : 0;
        if (sleep) {
            const s32 rc = sc(ctx, 85, {ps_read32(b, control + uwc_off::flag), mine, 0x11, 0,
                                           static_cast<u64>(static_cast<s64>(static_cast<s32>(timeout)))});
            if (rc != lv2::CELL_OK && static_cast<u32>(rc) != kLv2Timeout) fiber_assert(kFile, 338, kFn);
            if (r30 == 0 && rc != lv2::CELL_OK) break;
            r30 = r26;
            continue;
        }
        if (static_cast<u32>(others)) {
            const s32 rc = sc(ctx, 87, {ps_read32(b, control + uwc_off::flag), static_cast<u32>(others)});
            if (rc != lv2::CELL_OK && static_cast<u32>(rc) != kLv2Busy) fiber_assert(kFile, 346, kFn);
        }
        if (r30 == 0) break;
        // cellFiberPpuRunFibers(control) below this frame.
        const u64 rf = uwc - frame::run_fibers;
        ps_write64(b, static_cast<u32>(rf) + frame::run_fibers + 16, 0);
        if (!tls_block(ctx, g, tls_off::fiber) || ps_read32(b, tls_block(ctx, g, tls_off::fiber) + tls_off::fiber))
            fiber_assert(kFile, 350, kFn);
        ps_write64(b, control + sch_off::last_check, 0);
        ps_write8(b, static_cast<u32>(rf) + 112, 0);
        run_fibers(ctx, g, control, rf, kResumeWorker);
        fiber_assert(kFile, 350, kFn);
    }
    for (Reserve r;;) {
        const u32 w = r.lwarx(b, control + uwc_off::workers);
        if (r.stwcx(b, w & ~bitmap_bit)) break;
    }
    for (Reserve r;;) {
        const u64 c = r.ldarx(b, control + uwc_off::word);
        if (r.stdcx(b, (c & 0x00FFFFFFFFFFFFFFull) | (((c >> 56) - 1) << 56))) break;
    }
    return lv2::CELL_OK;
}

s32 cellFiberPpuUtilWorkerControlRunFibers(PPUContext& ctx, u32 control)
{
    if (!control) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (control & 0x7F) return err(CELL_FIBER_ERROR_ALIGN);
    if (!mapped(ctx, control, 0x300, "cellFiberPpuUtilWorkerControlRunFibers", "worker control"))
        return err(CELL_FIBER_ERROR_INVAL);
    FiberGlobals& g = globals();
    u8* b = gb(ctx);
    for (Reserve r;;) {
        const u64 c = r.ldarx(b, control + uwc_off::word);
        if (!(c & kUwcInitialized)) return err(CELL_FIBER_ERROR_STAT);
        if ((c >> 56) > 32) return err(CELL_FIBER_ERROR_AGAIN);
        if (r.stdcx(b, (c & 0x00FFFFFFFFFFFFFFull) | (((c >> 56) + 1) << 56))) break;
    }
    u32 index = 0;
    for (Reserve r;;) {
        const u32 w = r.lwarx(b, control + uwc_off::workers);
        index = static_cast<u32>(std::countl_zero(~w));
        if (r.stwcx(b, w | (index >= 32 ? 0 : 0x80000000u >> index))) break;
    }
    const u64 caller = ctx.gpr[1], uwc = caller - frame::worker;
    ps_write64(b, static_cast<u32>(caller) + 16, ctx.lr);
    ps_write32(b, static_cast<u32>(uwc) + 120, control);
    ps_write32(b, static_cast<u32>(uwc) + 124, index);
    return worker_loop(ctx, g, control, index, 0, uwc);
}

s32 worker_init(PPUContext& ctx, u32 control, u32 attr)
{
    if (!control) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (control & 0x7F) return err(CELL_FIBER_ERROR_ALIGN);
    if (!mapped(ctx, control, 0x300, "cellFiberPpuUtilWorkerControlInitialize", "worker control"))
        return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    std::memset(b + control, 0, 0x300);
    u32 id = 0;
    {
        // sys_event_flag_attribute_t: SYS_SYNC_PRIORITY, not process-shared, several waiters, "_fibUt".
        StackVar out(ctx, b, 8);
        StackVar a(ctx, b, 0x20);
        ps_write32(b, a.addr() + 0, 2);
        ps_write32(b, a.addr() + 4, 0x200);
        ps_write32(b, a.addr() + 0x14, 0x20000);
        put_text(b, a.addr() + 0x18, "_fibUt", 8);
        if (const s32 r = sc(ctx, 82, {out.addr(), a.addr(), 0}); r != lv2::CELL_OK) return r;
        id = ps_read32(b, out.addr());
    }
    ps_write32(b, control + uwc_off::flag, id);
    const u32 sdk = attr ? ps_read32(b, attr + 0x108) : 0;
    if (const s32 r = cellFiberPpuInitializeScheduler(ctx, control, attr); r != lv2::CELL_OK) {
        sc(ctx, 83, {id});
        return r;
    }
    ps_write8(b, control + uwc_off::word, 0);
    ps_write8(b, control + uwc_off::word + 1, 0);
    ps_write32(b, control + uwc_off::word + 4, 0);
    ps_write32(b, control + uwc_off::sdk, sdk);
    ps_write32(b, control + uwc_off::polling, 0);
    ps_write32(b, control + uwc_off::interval, 0);
    ps_write64(b, control + uwc_off::word, (ps_read64(b, control + uwc_off::word) & ~kUwcPolling) | kUwcInitialized);
    return lv2::CELL_OK;
}

s32 cellFiberPpuUtilWorkerControlInitialize(PPUContext& ctx, u32 control) { return worker_init(ctx, control, 0); }
s32 cellFiberPpuUtilWorkerControlInitializeWithAttribute(PPUContext& ctx, u32 control, u32 attr)
{
    return worker_init(ctx, control, attr);
}

s32 _cellFiberPpuUtilWorkerControlAttributeInitialize(PPUContext& ctx, u32 attr, u32 sdk)
{
    if (!attr) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (attr & 7) return err(CELL_FIBER_ERROR_ALIGN);
    if (!mapped(ctx, attr, 0x180, "_cellFiberPpuUtilWorkerControlAttributeInitialize", "attribute"))
        return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    std::memset(b + attr, 0, 0x180);
    ps_write32(b, attr + 0x108, sdk);
    return _cellFiberPpuSchedulerAttributeInitialize(ctx, attr, sdk);
}

s32 cellFiberPpuUtilWorkerControlShutdown(PPUContext& ctx, u32 control)
{
    if (!control) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (control & 0x7F) return err(CELL_FIBER_ERROR_ALIGN);
    if (!mapped(ctx, control, 0x300, "cellFiberPpuUtilWorkerControlShutdown", "worker control"))
        return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    u64 old = 0;
    for (Reserve r;;) {
        old = r.ldarx(b, control + uwc_off::word);
        if (!(old & kUwcInitialized)) return err(CELL_FIBER_ERROR_STAT);
        // Fibers left (libfiber's look at the ring, which writes nothing when the scheduler is not initialized).
        if (ps_read8(b, control + sch_off::initialized) &&
            ps_read32(b, control + sch_off::fibers) != control + sch_off::fibers)
            return err(CELL_FIBER_ERROR_BUSY);
        const u64 n = (old & ~(0xFFull << 40) & ~0xFFFFFFFFull & ~kUwcInitialized) | (1ull << 40);
        if (r.stdcx(b, n)) break;
    }
    if (static_cast<u32>(old)) {
        const s32 rc = sc(ctx, 87, {ps_read32(b, control + uwc_off::flag), static_cast<u32>(old)});
        if (rc != lv2::CELL_OK && static_cast<u32>(rc) != kLv2Busy)
            fiber_assert("fiber_ppu_util_worker_control.c", 145, "cellFiberPpuUtilWorkerControlShutdown", "libsre");
    }
    return lv2::CELL_OK;
}

s32 cellFiberPpuUtilWorkerControlFinalize(PPUContext& ctx, u32 control)
{
    if (!control) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (control & 0x7F) return err(CELL_FIBER_ERROR_ALIGN);
    if (!mapped(ctx, control, 0x300, "cellFiberPpuUtilWorkerControlFinalize", "worker control"))
        return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    if (ps_read64(b, control + uwc_off::word) & kUwcInitialized) return err(CELL_FIBER_ERROR_STAT);
    if (ps_read8(b, control + uwc_off::word)) return err(CELL_FIBER_ERROR_BUSY);
    constexpr std::string_view kFile = "fiber_ppu_util_worker_control.c", kFn = "cellFiberPpuUtilWorkerControlFinalize";
    if (cellFiberPpuFinalizeScheduler(ctx, control) != lv2::CELL_OK) fiber_assert(kFile, 172, kFn);
    if (sc(ctx, 83, {ps_read32(b, control + uwc_off::flag)}) != lv2::CELL_OK) fiber_assert(kFile, 175, kFn);
    return lv2::CELL_OK;
}

s32 cellFiberPpuUtilWorkerControlCheckFlags(PPUContext& ctx, u32 control, u32 wake)
{
    if (!control) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (control & 0x7F) return err(CELL_FIBER_ERROR_ALIGN);
    if (!mapped(ctx, control, 0x300, "cellFiberPpuUtilWorkerControlCheckFlags", "worker control"))
        return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    if (!(ps_read64(b, control + uwc_off::word) & kUwcInitialized)) return err(CELL_FIBER_ERROR_STAT);
    if (const s32 r = check_flags(ctx, control); r != lv2::CELL_OK) return r;
    if (!wake) return lv2::CELL_OK;
    bool runnable = false;
    {
        StackVar out(ctx, b, 8);
        has_runnable(ctx, control, out.addr());
        runnable = ps_read8(b, out.addr()) != 0;
    }
    return runnable ? worker_wakeup(ctx, control) : lv2::CELL_OK;
}

s32 cellFiberPpuUtilWorkerControlSendSignal(PPUContext& ctx, u32 fiber, u32 workers)
{
    if (!fiber) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (fiber & 0x7F) return err(CELL_FIBER_ERROR_ALIGN);
    if (!mapped(ctx, fiber, 0x380, "cellFiberPpuUtilWorkerControlSendSignal", "fiber")) return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 sched = ps_read32(b, fiber + fib_off::sched);
    if (const s32 r = signal_fiber(b, fiber, sched, 3); r != lv2::CELL_OK) return r;
    lwsync();
    const u32 threads = ps_read8(b, sched + sch_off::threads);
    if (ps_read32(b, fiber + fib_off::wake_control) == 1)
        return worker_wakeup(ctx, static_cast<u32>(ps_read64(b, fiber + fib_off::control)));
    if (workers) ps_write32(b, workers, threads);
    return lv2::CELL_OK;
}

s32 cellFiberPpuUtilWorkerControlSetPollingMode(PPUContext& ctx, u32 control, s32 mode, s32 interval)
{
    if (!control) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (control & 0x7F) return err(CELL_FIBER_ERROR_ALIGN);
    if (!mapped(ctx, control, 0x300, "cellFiberPpuUtilWorkerControlSetPollingMode", "worker control"))
        return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    if (mode == 0) {
        ps_write32(b, control + uwc_off::polling, 0);
        for (Reserve r;;) {
            const u64 c = r.ldarx(b, control + uwc_off::word);
            if (r.stdcx(b, c & ~kUwcPolling)) break;
        }
        return lv2::CELL_OK;
    }
    if (mode != 1 || interval == 0) return err(CELL_FIBER_ERROR_INVAL);
    ps_write32(b, control + uwc_off::polling, 1);
    ps_write32(b, control + uwc_off::interval, static_cast<u32>(interval));
    lwsync();
    u64 old = 0;
    for (Reserve r;;) {
        old = r.ldarx(b, control + uwc_off::word) | kUwcPolling;
        if (r.stdcx(b, (old & ~(0xFFull << 40) & ~0xFFFFFFFFull) | (1ull << 40))) break;
    }
    if (static_cast<u32>(old)) {
        const s32 rc = sc(ctx, 87, {ps_read32(b, control + uwc_off::flag), static_cast<u32>(old)});
        if (rc != lv2::CELL_OK && static_cast<u32>(rc) != kLv2Busy)
            fiber_assert("fiber_ppu_util_worker_control.c", 258, "cellFiberPpuUtilWorkerControlSetPollingMode");
    }
    return lv2::CELL_OK;
}

// cellFiberPpuUtilWorkerControlCreateFiber: cellFiberPpuCreateFiber with a copy of the attribute (a default one of
// SDK 3.3 without) that names the worker control to wake when the fiber gets a signal.
s32 cellFiberPpuUtilWorkerControlCreateFiber(PPUContext& ctx, u32 control, u32 fiber, u32 entry, u64 arg,
    u32 priority, u32 stack, u32 size, u32 attr)
{
    if (attr & 7) return err(CELL_FIBER_ERROR_ALIGN);
    u8* b = gb(ctx);
    StackVar local(ctx, b, 0x100);
    if (attr) {
        if (!mapped(ctx, attr, 0x100, "cellFiberPpuUtilWorkerControlCreateFiber", "attribute"))
            return err(CELL_FIBER_ERROR_INVAL);
        std::memmove(b + local.addr(), b + attr, 0x100);
    } else {
        _cellFiberPpuAttributeInitialize(ctx, local.addr(), 0x330000);
    }
    ps_write64(b, local.addr() + 0x40, control);
    ps_write32(b, local.addr() + 0x34, 1);
    return create_fiber(ctx, control, fiber, entry, arg, priority, stack, size, local.addr());
}

s32 cellFiberPpuUtilWorkerControlJoinFiber(PPUContext& ctx, u32, u32 fiber, u32 status)
{
    const u64 caller = ctx.gpr[1];
    ps_write64(gb(ctx), static_cast<u32>(caller) + 16, ctx.lr);
    return join_fiber(ctx, fiber, status, caller - frame::worker_join, kResumeWorkerJoinFiber);
}

s32 cellFiberPpuUtilWorkerControlConnectEventQueueToSpurs(PPUContext&) { return err(CELL_FIBER_ERROR_NOSYS); }
s32 cellFiberPpuUtilWorkerControlDisconnectEventQueue(PPUContext&) { return err(CELL_FIBER_ERROR_NOSYS); }

// --------------------------------------------------------------------------------- the scheduler traces

s32 trace_control(PPUContext& ctx, u32 sched, std::string_view fn, u32 lock_line, u32 unlock_line, u16 from, u16 to,
    bool clear)
{
    if (!sched) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if (sched & 0x7F) return err(CELL_FIBER_ERROR_ALIGN);
    if (!mapped(ctx, sched, 0x200, fn, "scheduler")) return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 m = sched + sch_off::mutex;
    lock_or_assert(ctx, m, "trace.c", lock_line, fn);
    s32 r = err(CELL_FIBER_ERROR_STAT);
    if (ps_read16(b, sched + sch_off::trace_mode) == from) {
        r = lv2::CELL_OK;
        if (clear) {
            ps_write16(b, sched + sch_off::trace_flags, 0);
            ps_write32(b, sched + sch_off::trace, 0);
        }
        ps_write16(b, sched + sch_off::trace_mode, to);
    }
    unlock_or_assert(ctx, m, "trace.c", unlock_line, fn);
    return r;
}

s32 cellFiberPpuSchedulerTraceInitialize(PPUContext& ctx, u32 sched, u32 buffer, u32 size, u32 mode)
{
    if (!sched || !buffer) return err(CELL_FIBER_ERROR_NULL_POINTER);
    if ((sched & 0x7F) || (buffer & 0xF)) return err(CELL_FIBER_ERROR_ALIGN);
    if (size <= 127 || (mode & ~3u) || mode == 0) return err(CELL_FIBER_ERROR_INVAL);
    const char* fn = "cellFiberPpuSchedulerTraceInitialize";
    if (!mapped(ctx, sched, 0x200, fn, "scheduler") || !mapped(ctx, buffer, 16, fn, "buffer")) return err(CELL_FIBER_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 m = sched + sch_off::mutex;
    lock_or_assert(ctx, m, "trace.c", 46, fn);
    s32 r = err(CELL_FIBER_ERROR_STAT);
    if (ps_read16(b, sched + sch_off::trace_mode) == 0) {
        ps_write32(b, sched + sch_off::trace, buffer);
        ps_write32(b, buffer + 4, (size - 0x80) >> 4);
        ps_write8(b, buffer + 8, 0);
        ps_write32(b, buffer, 0);
        ps_write16(b, sched + sch_off::trace_mode, 3);
        // libfiber's rotate pair keeps the mode but its bit 1 (the wrapping bit is bit 0).
        ps_write16(b, sched + sch_off::trace_flags, static_cast<u16>(std::rotl(std::rotl(mode, 30) & 0x7FFFFFFFu, 2)));
        r = lv2::CELL_OK;
    }
    unlock_or_assert(ctx, m, "trace.c", 63, fn);
    return r;
}

s32 cellFiberPpuSchedulerTraceFinalize(PPUContext& ctx, u32 sched)
{
    return trace_control(ctx, sched, "cellFiberPpuSchedulerTraceFinalize", 145, 157, 3, 0, true);
}
s32 cellFiberPpuSchedulerTraceStart(PPUContext& ctx, u32 sched)
{
    return trace_control(ctx, sched, "cellFiberPpuSchedulerTraceStart", 85, 95, 3, 2, false);
}
s32 cellFiberPpuSchedulerTraceStop(PPUContext& ctx, u32 sched)
{
    return trace_control(ctx, sched, "cellFiberPpuSchedulerTraceStop", 116, 126, 2, 3, false);
}

// _cellFiberPpuInitialize(tlsVariable): where libfiber's TLS block is, relative to r13 (r3 keeps the offset).
u32 _cellFiberPpuInitialize(PPUContext& ctx, u32 tls)
{
    const u32 offset = tls - static_cast<u32>(ctx.gpr[13]);
    globals().tls_offset.store(static_cast<s32>(offset), std::memory_order_relaxed);
    return offset;
}

} // namespace

void register_cellFiber(Registry& r)
{
    r.add(kModule, {
        PS_HLE_FUNC(0x01036193, cellFiberPpuContextReturnToThread),
        PS_HLE_FUNC(0x081C98BE, cellFiberPpuContextRunScheduler),
        PS_HLE_FUNC(0x0A25B6C8, cellFiberPpuContextEnterScheduler),
        PS_HLE_FUNC(0x0C44F441, cellFiberPpuYield),
        PS_HLE_FUNC(0x12B1ACF0, cellFiberPpuRunFibers),
        PS_HLE_FUNC(0x1E7A247A, cellFiberPpuUtilWorkerControlRunFibers),
        PS_HLE_FUNC(0x31252EC3, _cellFiberPpuContextAttributeInitialize),
        PS_HLE_FUNC(0x3204B146, cellFiberPpuUtilWorkerControlInitialize),
        PS_HLE_FUNC(0x34A81091, cellFiberPpuContextSelf),
        PS_HLE_FUNC(0x3860A12A, cellFiberPpuSchedulerTraceFinalize),
        PS_HLE_FUNC(0x392C5AA5, cellFiberPpuUtilWorkerControlSetPollingMode),
        PS_HLE_FUNC(0x3B417F82, cellFiberPpuUtilWorkerControlJoinFiber),
        PS_HLE_FUNC(0x4FC86B2C, cellFiberPpuUtilWorkerControlDisconnectEventQueue),
        PS_HLE_FUNC(0x55870804, _cellFiberPpuInitialize),
        PS_HLE_FUNC(0x5D3992DD, cellFiberPpuUtilWorkerControlSendSignal),
        PS_HLE_FUNC(0x5D9A7034, cellFiberPpuSelf),
        PS_HLE_FUNC(0x62A20F0D, cellFiberPpuUtilWorkerControlConnectEventQueueToSpurs),
        PS_HLE_FUNC(0x68BA4568, _cellFiberPpuUtilWorkerControlAttributeInitialize),
        PS_HLE_FUNC(0x6C164B3B, cellFiberPpuWaitSignal),
        PS_HLE_FUNC(0x72086315, cellFiberPpuContextInitialize),
        PS_HLE_FUNC(0x7C2F4034, cellFiberPpuCreateFiber),
        PS_HLE_FUNC(0x8AFB8356, cellFiberPpuSendSignal),
        PS_HLE_FUNC(0x8B6BAA01, cellFiberPpuFinalizeScheduler),
        PS_HLE_FUNC(0x9E25C72D, _cellFiberPpuSchedulerAttributeInitialize),
        PS_HLE_FUNC(0xA27C95CA, cellFiberPpuUtilWorkerControlFinalize),
        PS_HLE_FUNC(0xA4599CF3, cellFiberPpuWaitFlag),
        PS_HLE_FUNC(0xA6004249, cellFiberPpuJoinFiber),
        PS_HLE_FUNC(0xABA1C563, cellFiberPpuContextRun),
        PS_HLE_FUNC(0xADEDBEBF, cellFiberPpuSchedulerTraceStart),
        PS_HLE_FUNC(0xB0594B2D, cellFiberPpuGetScheduler),
        PS_HLE_FUNC(0xB3A48079, cellFiberPpuContextFinalize),
        PS_HLE_FUNC(0xB90C871B, cellFiberPpuContextCheckStackLimit),
        PS_HLE_FUNC(0xBABF714B, cellFiberPpuUtilWorkerControlWakeup),
        PS_HLE_FUNC(0xBF9CD933, cellFiberPpuSchedulerTraceInitialize),
        PS_HLE_FUNC(0xBFCA88D3, cellFiberPpuUtilWorkerControlCreateFiber),
        PS_HLE_FUNC(0xC04E2438, cellFiberPpuUtilWorkerControlShutdown),
        PS_HLE_FUNC(0xC11F8056, _cellFiberPpuAttributeInitialize),
        PS_HLE_FUNC(0xD0066B17, cellFiberPpuContextSwitch),
        PS_HLE_FUNC(0xE492A675, cellFiberPpuHasRunnableFiber),
        PS_HLE_FUNC(0xE665F9A9, cellFiberPpuSchedulerTraceStop),
        PS_HLE_FUNC(0xEA6DC1AD, cellFiberPpuUtilWorkerControlCheckFlags),
        PS_HLE_FUNC(0xEE3B604D, cellFiberPpuInitializeScheduler),
        PS_HLE_FUNC(0xF2CCAD4F, cellFiberPpuUtilWorkerControlInitializeWithAttribute),
        PS_HLE_FUNC(0xF3E81219, cellFiberPpuCheckStackLimit),
        PS_HLE_FUNC(0xF6C6900C, cellFiberPpuCheckFlags),
        PS_HLE_FUNC(0xFA8D5F95, cellFiberPpuExit),
        PS_HLE_FUNC(0xFBF5FE40, cellFiberPpuSetPriority),
        PS_HLE_INTERNAL(0x6D4EFCB9, _pspocket_cellFiberPpuContextStart),
        PS_HLE_INTERNAL(0xA6B642C9, _pspocket_cellFiberPpuContextEntryReturned),
        PS_HLE_INTERNAL(0xB7C31F9C, _pspocket_cellFiberPpuFiberEntry),
        PS_HLE_INTERNAL(0xA04AC02F, _pspocket_cellFiberPpuFiberReturned),
        PS_HLE_INTERNAL(0x4D8BBEDA, _pspocket_cellFiberPpuReschedule),
        PS_HLE_INTERNAL(0xE21FFD2A, _pspocket_cellFiberPpuOnExitJoinFiber),
        PS_HLE_INTERNAL(0x5A0BDFCC, _pspocket_cellFiberPpuOnExitJoinThread),
        PS_HLE_INTERNAL(0xD0D56F4E, _pspocket_cellFiberPpuResumeThread),
        PS_HLE_INTERNAL(0x32D33CC0, _pspocket_cellFiberPpuResumeRunFibers),
        PS_HLE_INTERNAL(0x18C6A704, _pspocket_cellFiberPpuResumeWorker),
        PS_HLE_INTERNAL(0x0BAC1E49, _pspocket_cellFiberPpuResumeEnterScheduler),
        PS_HLE_INTERNAL(0xA403DFE4, _pspocket_cellFiberPpuResumeYield),
        PS_HLE_INTERNAL(0x0887393E, _pspocket_cellFiberPpuResumeWaitSignal),
        PS_HLE_INTERNAL(0x705886B9, _pspocket_cellFiberPpuResumeJoinFiber),
        PS_HLE_INTERNAL(0x5B96C141, _pspocket_cellFiberPpuResumeWorkerJoinFiber),
        PS_HLE_INTERNAL(0x32A86963, _pspocket_cellFiberPpuResumeWaitFlag),
        PS_HLE_INTERNAL(0x7498D0CF, _pspocket_cellFiberPpuResumeExit),
        PS_HLE_INTERNAL(0xE9A2B302, _pspocket_cellFiberPpuResumeSwitch),
        PS_HLE_INTERNAL(0x0DB9AD81, _pspocket_cellFiberPpuResumeReturnToThread),
    });
}

} // namespace pspocket::hle
