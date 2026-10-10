// cellSpursJq (D-537): the SPURS job queue, libspurs_jq.sprx's PPU side (fw 4.92) as pspocket's own, read from Sony's
// code (the reference, D-046) and compared with it running on pspocket (tests/hle/test_spurs_jq_compare.cpp). RPCS3's
// rpcs3/Emu/Cell/Modules/cellSpursJq.cpp registers the functions as todo stubs and gives nothing else.
//
// A job queue is a SPURS workload whose policy module is libspurs_jq's SPU half: it pops 8-byte commands (a job, a job
// list, a job to give back to the descriptor pool, FLUSH, SYNC) from a ring in the game's command list, runs the jobs,
// releases their semaphores and wakes the PPU threads waiting for room. That SPU program is Sony's and runs as SPU code
// on pspocket's SPURS like any game's own policy module (spurs_custom.cpp). pspocket takes it at run time from the
// user's installed system software (/dev_flash/sys/external/libspurs_jq.sprx, found through the module's own table of
// contents) and never keeps it in the repository (CLAUDE.md rule 1). Everything the PPU does is here:
// - the queue (CellSpursJobQueue, 4 KB) laid out where libspurs_jq lays it out, made with the same lv2 objects (two
//   event flags, two lightweight mutexes, a lightweight condition: libspurs_jq's "_sreJQ"), the same workload attribute
//   and exception event handler;
// - the command ring's protocol with the policy module, every update the ldarx/stdcx. or lwarx/stwcx. libspurs_jq makes,
//   through the reservation system as the PPU interpreter and lifted code make them (class Reserve), so a store by any
//   processor in between fails it as on the hardware. Two pushers: the default one, where pushers in flight count
//   themselves in the header, claim the next free entry of their lap with a conditional store and the last one in moves
//   the write index, and the "submit with entry lock" one, which takes the entry with the header's own store;
// - pushers that find the ring full wait in the waiting ring after the command list: a PPU thread on a bit of one of the
//   queue's two event flags (128 bits), a fiber through cellFiber; the policy module wakes them as it makes room;
// - the job descriptor pool (_cellSpursCreateJobQueueWithJobDescriptorPool): eight size classes of descriptors, each a
//   lock-free list with its own waiting ring; semaphores (a 29-bit count and one waiter), ports and port2s.
// libspurs_jq's own slips are kept where they show (each marked in the code): creation does not clean up after a failed
// cellSpursSetExceptionEventHandler, a semaphore acquire that takes event flag bit 0 never gives it back, an old-SDK
// multi-thread port sync stores a word of its doubleword through stwcx., a port initialized when Open fails keeps the
// handle the stack held.
//
// Deviations: libspurs_jq's assertions (its "PS3 SDK INTERNAL ASSERTION FAILURE" and abort) are fatal logs with the same
// text. Objects that are not guest memory are refused with CELL_SPURS_JOB_ERROR_INVAL and the call site logged, where
// libspurs_jq would fault. libspurs_jq compares the title's ID with a list of ten titles that get policy module
// workarounds (CellSpursJobQueue + 0xA0); the list is its data and is not reproduced (as libssl's, D-524): the PARAM.SFO
// read is made, no title matches.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/core/reservation.h"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/self_decrypt.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellSpursJq";
constexpr std::string_view kModule = "cellSpursJq";

enum : u32 {
    CELL_SPURS_JOB_ERROR_AGAIN = 0x80410A01,
    CELL_SPURS_JOB_ERROR_INVAL = 0x80410A02,
    CELL_SPURS_JOB_ERROR_PERM = 0x80410A09,
    CELL_SPURS_JOB_ERROR_BUSY = 0x80410A0A,
    CELL_SPURS_JOB_ERROR_STAT = 0x80410A0F,
    CELL_SPURS_JOB_ERROR_ALIGN = 0x80410A10,
    CELL_SPURS_JOB_ERROR_NULL_POINTER = 0x80410A11,
    CELL_SPURS_JOB_ERROR_MEMORY_SIZE = 0x80410A17,
    // What cellSpurs answers that libspurs_jq turns into its own codes.
    CELL_SPURS_POLICY_MODULE_ERROR_AGAIN = 0x80410801,
    CELL_SPURS_POLICY_MODULE_ERROR_INVAL = 0x80410802,
    CELL_SPURS_POLICY_MODULE_ERROR_STAT = 0x8041080F,
};
s32 err(u32 code) { return static_cast<s32>(code); }

// libspurs_jq's own version, its SDK version, as its assertions print it and its workload attribute carries it.
constexpr u32 kLibraryVersion = 0x492000;
constexpr u32 kLibrarySdkVersion = 0x330000;

// CellSpursJobQueueAttribute (libspurs_jq writes and reads these fields only).
namespace qattr {
constexpr u32 maxGrab = 0x04, maxSizeJobDescriptor = 0x08, submitWithEntryLock = 0x0C, doBusyWaiting = 0x0D,
              isHaltOnError = 0x0E, isJobTypeMemoryCheck = 0x0F, maxNumJobsOnASpu = 0x10;
}

// CellSpursJobQueue (4 KB, 128-aligned): where libspurs_jq keeps what, its SPU half included (the fields it updates).
namespace jq_off {
// The command ring's header (u64): read index (u16 at 0), write index (u16 at 2), pushers in flight (byte 4), wake-ups
// granted to waiters (byte 5), waiters (byte 6), the read lap (bit 6) and the write lap (bit 5) in byte 7.
constexpr u32 header = 0x00;
// The waiting ring's header (u64): write index (bits 47..32), pushers in flight (byte 4, the u8 at 0x0C), byte 5 (0x0D),
// the lap (byte 6, 0x0E).
constexpr u32 wait_header = 0x08;
constexpr u32 command_list = 0x10, depth = 0x14, wait_depth = 0x16;
// The control doubleword: byte 0 (the policy module's), maxGrab (byte 5, 0x65), busy waiting (0x4000), halt on error
// (0x1000), running (0x800).
constexpr u32 control = 0x60, max_grab = 0x65;
constexpr u32 signal_list = 0x68; // jobs cellSpursJobQueueSendSignal woke, linked through their first word
constexpr u32 spurs_wid = 0x6C;   // the CellSpurs address | the workload ID (0x20: none)
constexpr u32 handles = 0x70;     // u16: open handles
constexpr u32 submit_with_entry_lock = 0x7A, max_jobs_on_spu = 0x7B;
constexpr u32 event_flag0 = 0x80, event_flag1 = 0x84;
constexpr u32 wait_bits = 0x88;   // 128 bits: event flag bits PPU threads wait on
constexpr u32 revision = 0x9C, one_1c4 = 0x1C4; // 0xA0: libspurs_jq's title workarounds (left 0)
constexpr u32 handle_bits = 0x580; // 1024 bits: open handles
// The descriptor pool: class starts, ends, waiting ring entries (8 u32 each); the pool's start and the descriptors' end;
// then a 16-byte record per class: its free list (head u32, count u16, wake-ups byte 6, waiters byte 7) and its
// waiting ring's header.
constexpr u32 pool_start = 0x600, pool_end = 0x620, pool_wait = 0x640, pool_base = 0x660, pool_limit = 0x664,
              pool_class = 0x680;
constexpr u32 exit_code = 0xF04, cause = 0xF08, spurs = 0xF10, max_size = 0xF14, memory_check = 0xF18;
constexpr u32 lwmutex0 = 0xF20, lwmutex1 = 0xF38, lwcond = 0xF50;
constexpr u32 handler = 0xFE8, handler_arg = 0xFF0, sdk_version = 0xFF8, jq_revision = 0xFFC;
constexpr u32 size = 0x1000;
} // namespace jq_off

// CellSpursJobQueueSemaphore (16 bytes used, 128-aligned): a doubleword (a waiter: bit 63, its kind in bits 62..61 (0 a
// fiber, 2 a PPU thread), its fiber or event flag bit in the low word; a signed 29-bit count in bits 60..32), then the
// queue.
namespace sem_off {
constexpr u32 queue = 0x08;
}
// CellSpursJobQueuePort (128 bytes, 128-aligned): a semaphore, then the jobs it counts (u32 at 0x10), the next slot of
// the copy buffer (u16 at 0x14), busy (byte 0x16) and byte 0x17 in one doubleword; the copy buffer, its slots and their
// size; the queue, the handle; initialized, multi-threaded.
namespace port_off {
constexpr u32 counter = 0x10, count = 0x10, slot = 0x14, buffer = 0x18, slots = 0x1C, slot_size = 0x1E,
              queue = 0x20, handle = 0x24, initialized = 0x28, multi = 0x2A, size = 0x80;
}
// CellSpursJobQueuePort2 (128 bytes): a semaphore, the jobs it counts (u32 at 0x10) and busy (byte 0x14) in one
// doubleword, the queue, the handle, created (0x20).
namespace port2_off {
constexpr u32 counter = 0x10, count = 0x10, queue = 0x18, handle = 0x1C, created = 0x20, byte21 = 0x21,
              size = 0x80;
}

// The sizes of the pool's eight descriptor classes (libspurs_jq's table; CellSpursJob64 .. CellSpursJob896).
constexpr u32 kClassSize[8] = {0x40, 0x80, 0x100, 0x180, 0x200, 0x280, 0x300, 0x380};

// Commands (the high word's low three bits): a job, a job list, a pool job to give back after it ran, an extension
// (FLUSH 3, SYNC 7 with the tag mask in the top 16 bits). The tag in bits 38..35, the semaphore (128-aligned) above it,
// and the descriptor (16-aligned) in the low word with its size / 128 in bits 3..1; bit 34 a job's second flag.
constexpr u64 kCmdJobList = 1ull << 32, kCmdRelease = 2ull << 32, kCmdFlush = 3ull << 32, kCmdSync = 7ull << 32;
constexpr u64 kWaitThread = 0x02000000; // a waiting ring entry's PPU thread mark (its event flag bit above)

// ------------------------------------------------------------------------------------------------ guest helpers

u8* gb(PPUContext& ctx) { return guest_base(ctx); }

void lwsync() { std::atomic_thread_fence(std::memory_order_acq_rel); }

// The PPU's ldarx/lwarx and stdcx./stwcx. (as cellSync2's): a snapshot of the 128-byte line and its version, then a store
// that succeeds only if neither changed.
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
    // stwcx. to the reserved doubleword's address (an old-SDK port sync: see port_sync).
    bool stwcx_at(u8* base, u32 addr, u32 value) const
    {
        return ps_res_store_conditional(base, addr, 4, value, version_, line_);
    }

private:
    alignas(16) u8 line_[PS_RES_LINE_SIZE];
    u64 version_ = 0;
    u32 addr_ = 0;
};

// A plain store to memory the policy module shares breaks reservations on it as on the hardware.
void stored(u32 addr, u32 size) { ps_res_notify_write(addr, size); }
void st64(u8* b, u32 a, u64 v)
{
    ps_write64(b, a, v);
    stored(a, 8);
}
void st32(u8* b, u32 a, u32 v)
{
    ps_write32(b, a, v);
    stored(a, 4);
}
void st16(u8* b, u32 a, u16 v)
{
    ps_write16(b, a, v);
    stored(a, 2);
}
void zero(u8* b, u32 a, u32 n)
{
    std::memset(b + a, 0, n);
    stored(a, n);
}

// Whether [addr, addr + size) is guest memory, page by page.
bool guest_range(lv2::PpuThread& t, u32 addr, u64 size)
{
    if (!size) return true;
    if (u64{addr} + size > GuestMemory::kGuestSize) return false;
    const u64 page = GuestMemory::host_page_size();
    for (u64 a = addr & ~(page - 1); a < u64{addr} + size; a += page)
        if (!t.kernel.memory().is_committed(static_cast<u32>(a))) return false;
    return true;
}

// Memory libspurs_jq would read or write that is not the guest's: refused, the call site logged (D-042).
bool mapped(PPUContext& ctx, u32 addr, u64 size, std::string_view fn, std::string_view what)
{
    if (guest_range(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, gb(ctx), std::format("{}: the {} at {:#x} ({:#x} bytes) is not guest memory; "
                                            "CELL_SPURS_JOB_ERROR_INVAL", fn, what, addr, size));
    return false;
}

// libspurs_jq's assertion: its message, then the process ends.
[[noreturn]] void jq_assert(std::string_view file, u32 line, std::string_view function)
{
    PS_FATAL(kChan, "PS3 SDK INTERNAL ASSERTION FAILURE: libspurs_jq, version {:x}, file {}, line {}, function \"{}\"",
        kLibraryVersion, file, line, function);
}

// ---------------------------------------------------------------------------------------------- other libraries

// libspurs_jq's imports (NIDs), and what each comes from.
constexpr u32 kAddWorkloadWithAttribute = 0xC0158D8B, kWorkloadAttributeInitialize = 0xEFEB2679,
              kWorkloadAttributeSetName = 0x4A5EAB63, kSetExceptionEventHandler = 0xD2E23FA9,
              kSendWorkloadSignal = 0x1D2BCA4B, kWakeUp = 0x7E4EA023, kShutdownWorkload = 0x98D5B343,
              kWaitForWorkloadShutdown = 0x5FD43FE4, kRemoveWorkload = 0x57E4DEC3;
constexpr u32 kFiberSelf = 0x5D9A7034, kFiberWaitSignal = 0x6C164B3B;
constexpr u32 kLwmutexCreate = 0x2F85C0EF, kLwmutexDestroy = 0xC3476D0C, kLwcondCreate = 0xDA0EB71A,
              kLwcondDestroy = 0x1C9A942C, kGetParamsfo = 0xE75C40F2;

// A function of another library libspurs_jq imports: the firmware module's when the process has it in place of
// pspocket's (Registry::add_imports), else pspocket's own, with up to eight arguments in registers and the rest in a
// fresh frame's parameter save area (as call_guest lays them out). `found` false when the process has neither.
u64 call_import(PPUContext& ctx, std::string_view library, u32 nid, std::initializer_list<u64> args, bool* found = nullptr)
{
    if (found) *found = true;
    if (const u32 opd = HleProcess::current()->imported(library, nid)) return call_guest(ctx, opd, args);
    const Function* f = registry().find(library, nid);
    if (!f) {
        if (found) *found = false;
        return 0;
    }
    u8* base = gb(ctx);
    const u64 saved_r1 = ctx.gpr[1];
    const u32 extra = args.size() > 8 ? static_cast<u32>(args.size()) - 8 : 0;
    if (extra) {
        ctx.gpr[1] = (saved_r1 - 0x80 - 8 * extra) & ~u64{15};
        ps_write64(base, static_cast<u32>(ctx.gpr[1]), saved_r1);
    }
    u32 i = 0;
    for (const u64 a : args) {
        if (i < 8) ctx.gpr[3 + i] = a;
        else ps_write64(base, static_cast<u32>(ctx.gpr[1]) + 0x70 + 8 * (i - 8), a);
        i++;
    }
    f->fn(ctx, base);
    ctx.gpr[1] = saved_r1;
    return ctx.gpr[3];
}

s32 spurs_call(PPUContext& ctx, u32 nid, std::initializer_list<u64> args)
{
    return static_cast<s32>(call_import(ctx, "cellSpurs", nid, args));
}
s32 lv2_call(PPUContext& ctx, u32 nid, std::initializer_list<u64> args)
{
    return static_cast<s32>(call_import(ctx, "sysPrxForUser", nid, args));
}
s32 sc(PPUContext& ctx, u32 number, std::initializer_list<u64> args)
{
    return static_cast<s32>(lv2_syscall(ctx, number, args));
}

// cellFiberPpuSelf: the fiber running on this thread, 0 on a PPU thread (and with no cellFiber in the process).
u32 fiber_self(PPUContext& ctx) { return static_cast<u32>(call_import(ctx, "cellFiber", kFiberSelf, {})); }
s32 fiber_wait_signal(PPUContext& ctx) { return static_cast<s32>(call_import(ctx, "cellFiber", kFiberWaitSignal, {})); }

// cellSpursSendWorkloadSignal and cellSpursWakeUp for the queue's workload (the push's kick).
s32 kick(PPUContext& ctx, u8* b, u32 jq)
{
    const u32 spurs = ps_read32(b, jq + jq_off::spurs);
    spurs_call(ctx, kSendWorkloadSignal, {spurs, ps_read32(b, jq + jq_off::spurs_wid) & 0x7F});
    return spurs_call(ctx, kWakeUp, {spurs});
}

// ----------------------------------------------------------------------------- the policy module, from /dev_flash

// What a process keeps for libspurs_jq: the policy module's guest copy and its size word, the class name of the
// workload ("JobQueue"), and the guest strings the exception handler prints with.
struct JqGlobals {
    std::mutex mutex;
    bool tried = false;
    u32 pm = 0, pm_size = 0;
    u32 strings = 0; // a guest block of the module's text (see kStrings)
};

JqGlobals& globals() { return HleProcess::current()->state<JqGlobals>(); }

constexpr std::string_view kModuleFile = "/dev_flash/sys/external/libspurs_jq.sprx";

// Where the job queue's policy module is in the decrypted module: its table of contents (the module info's TOC word, at
// +0x20) holds the image's size and then its address, the addresses unrelocated (the module's text starts at 0). The
// image is SPU code at a 128-byte boundary of the text that starts with an SPU GUID (four `ila $2`, the SDK's
// .SpuGUID), and the size word runs to within the text. Nothing of the module enters pspocket: the pair is found by
// shape in the user's own file.
// *file_offset gets the image's offset in `elf_bytes`.
bool find_policy_module(std::span<const u8> elf_bytes, u64& file_offset, u32& size, std::string& why)
{
    auto elf = recomp::ElfFile::parse(elf_bytes);
    if (!elf) {
        why = elf.error().message;
        return false;
    }
    std::vector<const recomp::ProgramHeader*> loads;
    for (const recomp::ProgramHeader& ph : elf->segments())
        if (ph.type == 1 && ph.data_in_file) loads.push_back(&ph);
    if (loads.size() < 2) {
        why = "fewer than two loadable segments";
        return false;
    }
    const recomp::ProgramHeader& text = *loads[0];
    const std::span<const u8> text_bytes = elf->segment_data(text);
    const u64 info = text.paddr - text.offset; // the module info, in the text (as the loader finds it)
    if (info + 0x24 > text_bytes.size()) {
        why = "no module info";
        return false;
    }
    const u32 toc = ps_ld32(text_bytes.data() + info + 0x20);
    const auto word_at = [&](u64 vaddr, u32& out) {
        const auto bytes = elf->read_vaddr(vaddr, 4);
        if (!bytes) return false;
        out = ps_ld32(bytes->data());
        return true;
    };
    const auto is_guid = [&](u32 at) {
        if (at % 128 || u64{at} + 16 > text_bytes.size()) return false;
        for (u32 i = 0; i < 4; i++)
            if ((ps_ld32(text_bytes.data() + at + 4 * i) & 0xFE00007Fu) != 0x42000002u) return false; // ila $2, ...
        return true;
    };
    for (u64 slot = toc - 0x8000u; slot + 8 <= u64{toc} + 0x8000u; slot += 4) {
        u32 count = 0, at = 0;
        if (!word_at(slot, count) || !word_at(slot + 4, at)) break;
        if (count > 0x4000 && u64{at} + count <= text_bytes.size() && is_guid(at)) {
            file_offset = text.offset + at;
            size = count;
            return true;
        }
    }
    why = "no SPU image with a size in the table of contents";
    return false;
}

// The module's text libspurs_jq's exception handler prints with (its own words, kept in a guest block at first use).
constexpr std::string_view kStrings[] = {
    "JobQueue",
};
constexpr u32 kStringJobQueue = 0;

// The policy module's guest copy (loaded once per process): its address, and its size word in *size. 0 when the
// firmware module is not there or holds no such image (logged).
u32 policy_module(PPUContext& ctx, u32& size)
{
    JqGlobals& g = globals();
    std::lock_guard lock(g.mutex);
    if (!g.tried) {
        g.tried = true;
        lv2::Kernel& k = thread_of(ctx).kernel;
        std::vector<u8> file;
        std::string why;
        u64 offset = 0;
        u32 count = 0;
        if (!k.vfs_read_file(kModuleFile, file)) {
            why = std::format("{} cannot be read (no system software installed?)", kModuleFile);
        } else if (auto elf = recomp::decrypt_self(file, recomp::builtin_self_keys()); !elf) {
            why = std::format("{} does not decrypt: {}", kModuleFile, elf.error().message);
        } else if (!find_policy_module(*elf, offset, count, why)) {
            why = std::format("{}: {}", kModuleFile, why);
        } else {
            // The policy module DMAs itself in whole 128-byte blocks: its copy is kept that long.
            const u32 span = (count + 127) & ~127u;
            const u32 at = HleProcess::current()->main_memory().alloc(span, 0x80);
            if (!at) {
                why = std::format("no guest memory for the policy module ({:#x} bytes)", span);
            } else {
                std::memcpy(gb(ctx) + at, elf->data() + offset, count);
                g.pm = at;
                g.pm_size = count;
                PS_LOG_INFO(kChan, "the job queue's policy module: {:#x} bytes of {} at {:#x}", count, kModuleFile, at);
            }
        }
        if (!g.pm) PS_LOG_ERROR(kChan, "no job queue policy module: {}", why);
        u32 bytes = 0;
        for (const std::string_view s : kStrings) bytes += static_cast<u32>(s.size()) + 1;
        g.strings = HleProcess::current()->main_memory().alloc((bytes + 15) & ~15u, 0x10);
        if (g.strings) {
            u32 at = g.strings;
            for (const std::string_view s : kStrings) {
                std::memcpy(gb(ctx) + at, s.data(), s.size());
                ps_write8(gb(ctx), at + static_cast<u32>(s.size()), 0);
                at += static_cast<u32>(s.size()) + 1;
            }
        }
    }
    size = g.pm_size;
    return g.pm;
}

// The guest address of one of kStrings.
u32 string_of(u32 index)
{
    JqGlobals& g = globals();
    std::lock_guard lock(g.mutex);
    if (!g.strings) return 0;
    u32 at = g.strings;
    for (u32 i = 0; i < index; i++) at += static_cast<u32>(kStrings[i].size()) + 1;
    return at;
}

// The exception event handler's descriptor (an internal function of this module).
u32 exception_handler_descriptor()
{
    constexpr std::string_view kName = "_pspocket_cellSpursJqExceptionHandler";
    HleProcess* p = HleProcess::current();
    const u32 nid = recomp::nid_of(kName);
    if (const u32 d = p->descriptor_of(kModule, nid)) return d;
    return p->link_function(kModule, nid, kName);
}

// ------------------------------------------------------------------------------------------------ attributes

s32 cellSpursJobQueueAttributeInitialize(PPUContext& ctx, u32 attr)
{
    if (!attr) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (!mapped(ctx, attr, 0x14, "cellSpursJobQueueAttributeInitialize", "attribute")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    ps_write32(b, attr + qattr::maxNumJobsOnASpu, 0xFF);
    ps_write32(b, attr + qattr::maxGrab, 4);
    ps_write32(b, attr + qattr::maxSizeJobDescriptor, 0x100);
    ps_write8(b, attr + qattr::isJobTypeMemoryCheck, 0);
    ps_write8(b, attr + qattr::submitWithEntryLock, 0);
    ps_write8(b, attr + qattr::doBusyWaiting, 0);
    ps_write8(b, attr + qattr::isHaltOnError, 0);
    return lv2::CELL_OK;
}

s32 cellSpursJobQueueAttributeSetMaxGrab(PPUContext& ctx, u32 attr, u32 max_grab)
{
    if (!attr) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (max_grab - 1 >= 16) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!mapped(ctx, attr, 0x14, "cellSpursJobQueueAttributeSetMaxGrab", "attribute")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    ps_write32(gb(ctx), attr + qattr::maxGrab, max_grab);
    return lv2::CELL_OK;
}

// The four flag setters: the flag's byte (the argument's low byte, as libspurs_jq stores it).
s32 set_flag(PPUContext& ctx, u32 attr, u32 at, u8 value, std::string_view fn)
{
    if (!attr) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (!mapped(ctx, attr, 0x14, fn, "attribute")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    ps_write8(gb(ctx), attr + at, value);
    return lv2::CELL_OK;
}
s32 cellSpursJobQueueAttributeSetSubmitWithEntryLock(PPUContext& ctx, u32 attr, u8 v)
{
    return set_flag(ctx, attr, qattr::submitWithEntryLock, v, "cellSpursJobQueueAttributeSetSubmitWithEntryLock");
}
s32 cellSpursJobQueueAttributeSetDoBusyWaiting(PPUContext& ctx, u32 attr, u8 v)
{
    return set_flag(ctx, attr, qattr::doBusyWaiting, v, "cellSpursJobQueueAttributeSetDoBusyWaiting");
}
s32 cellSpursJobQueueAttributeSetIsHaltOnError(PPUContext& ctx, u32 attr, u8 v)
{
    return set_flag(ctx, attr, qattr::isHaltOnError, v, "cellSpursJobQueueAttributeSetIsHaltOnError");
}
s32 cellSpursJobQueueAttributeSetIsJobTypeMemoryCheck(PPUContext& ctx, u32 attr, u8 v)
{
    return set_flag(ctx, attr, qattr::isJobTypeMemoryCheck, v, "cellSpursJobQueueAttributeSetIsJobTypeMemoryCheck");
}

s32 cellSpursJobQueueAttributeSetMaxSizeJobDescriptor(PPUContext& ctx, u32 attr, u32 size)
{
    if (!attr) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (size - 0x100 > 0x2FF || (size & 0x7F)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!mapped(ctx, attr, 0x14, "cellSpursJobQueueAttributeSetMaxSizeJobDescriptor", "attribute"))
        return err(CELL_SPURS_JOB_ERROR_INVAL);
    ps_write32(gb(ctx), attr + qattr::maxSizeJobDescriptor, size);
    return lv2::CELL_OK;
}

s32 cellSpursJobQueueAttributeSetGrabParameters(PPUContext& ctx, u32 attr, u32 max_jobs_on_spu, u32 max_grab)
{
    if (!attr) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (max_jobs_on_spu > 0xFF || max_grab - 1 > 0xF) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!mapped(ctx, attr, 0x14, "cellSpursJobQueueAttributeSetGrabParameters", "attribute"))
        return err(CELL_SPURS_JOB_ERROR_INVAL);
    ps_write32(gb(ctx), attr + qattr::maxGrab, max_grab);
    ps_write32(gb(ctx), attr + qattr::maxNumJobsOnASpu, max_jobs_on_spu);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------- creation, end

// The lv2 objects' name: libspurs_jq's "_sreJQ", eight bytes with the last one 0.
void write_name(u8* b, u32 at)
{
    static constexpr char kName[8] = {'_', 's', 'r', 'e', 'J', 'Q', 0, 0};
    std::memcpy(b + at, kName, 8);
}

// Destroys what a failed creation made (libspurs_jq's own order), then turns cellSpurs' answer into the queue's.
s32 undo_create(PPUContext& ctx, u8* b, u32 jq, s32 r)
{
    sc(ctx, 83, {ps_read32(b, jq + jq_off::event_flag0)});
    sc(ctx, 83, {ps_read32(b, jq + jq_off::event_flag1)});
    lv2_call(ctx, kLwmutexDestroy, {jq + jq_off::lwmutex0});
    lv2_call(ctx, kLwmutexDestroy, {jq + jq_off::lwmutex1});
    lv2_call(ctx, kLwcondDestroy, {jq + jq_off::lwcond});
    switch (static_cast<u32>(r)) {
    case CELL_SPURS_POLICY_MODULE_ERROR_INVAL: return err(CELL_SPURS_JOB_ERROR_INVAL);
    case CELL_SPURS_POLICY_MODULE_ERROR_STAT: return err(CELL_SPURS_JOB_ERROR_STAT);
    case CELL_SPURS_POLICY_MODULE_ERROR_AGAIN: return err(CELL_SPURS_JOB_ERROR_AGAIN);
    default: return r;
    }
}

// _cellSpursCreateJobQueueWithJobDescriptorPool(revision, sdkVersion, spurs, jobQueue, attr, pool, poolDescription,
// name, commandList, depth, maxContention, priorities).
s32 _cellSpursCreateJobQueueWithJobDescriptorPool(PPUContext& ctx, u32 jq_revision, u32 sdk_version, u32 spurs, u32 jq,
    u32 attr, u32 pool, u32 pool_desc, u32 name, u32 list, u32 depth, u32 max_contention, u32 priorities)
{
    if (!spurs || !jq || !attr || !list) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (pool ? !pool_desc : pool_desc != 0) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if ((spurs & 0x7F) || (jq & 0x7F) || (list & 0x7F) || (pool && (pool & 0x7F)))
        return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (depth > 0x8000 || max_contention > 8) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    {
        // sys_memory_get_page_attribute: the queue in memory SPU threads may access.
        StackVar page(ctx, b, 0x18);
        if (sc(ctx, 351, {jq, page.addr()}) != lv2::CELL_OK || !(ps_read64(b, page.addr() + 8) & 2))
            return err(CELL_SPURS_JOB_ERROR_INVAL);
    }
    const char* fn = "_cellSpursCreateJobQueueWithJobDescriptorPool";
    if (!mapped(ctx, attr, 0x14, fn, "attribute")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (ps_read32(b, attr + qattr::maxGrab) - 1 > 0xF) return err(CELL_SPURS_JOB_ERROR_INVAL);
    const u32 max_size = ps_read32(b, attr + qattr::maxSizeJobDescriptor);
    if (max_size <= 0xFF || max_size > 0x3FF || (max_size & 0x7F)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (pool_desc && !mapped(ctx, pool_desc, 0x20, fn, "pool description")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (pool_desc && (ps_read32(b, pool_desc) & 1)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    const u32 list_size = (((depth + 15) << 3) & 0xFFFFFF80u) + 0x800;
    if (!mapped(ctx, jq, jq_off::size, fn, "job queue") || !mapped(ctx, list, list_size, fn, "command list"))
        return err(CELL_SPURS_JOB_ERROR_INVAL);
    zero(b, jq, jq_off::size);
    zero(b, list, list_size);

    {
        // sys_event_flag_attribute_t: priority, not shared, no IPC key, multiple waiters, "_sreJQ".
        StackVar ef(ctx, b, 0x20);
        ps_write32(b, ef.addr() + 0x00, 2);
        ps_write32(b, ef.addr() + 0x04, 0x200);
        ps_write32(b, ef.addr() + 0x14, 0x20000);
        write_name(b, ef.addr() + 0x18);
        if (const s32 r = sc(ctx, 82, {jq + jq_off::event_flag0, ef.addr(), 0}); r != lv2::CELL_OK) return r;
        if (const s32 r = sc(ctx, 82, {jq + jq_off::event_flag1, ef.addr(), 0}); r != lv2::CELL_OK) return r;
    }
    {
        // sys_lwmutex_attribute_t: priority, not recursive, "_sreJQ"; sys_lwcond_attribute_t: "_sreJQ".
        StackVar mattr(ctx, b, 0x10);
        ps_write32(b, mattr.addr() + 0x00, 2);
        ps_write32(b, mattr.addr() + 0x04, 0x20);
        write_name(b, mattr.addr() + 0x08);
        if (const s32 r = lv2_call(ctx, kLwmutexCreate, {jq + jq_off::lwmutex0, mattr.addr()}); r != lv2::CELL_OK) return r;
        if (const s32 r = lv2_call(ctx, kLwmutexCreate, {jq + jq_off::lwmutex1, mattr.addr()}); r != lv2::CELL_OK) return r;
        StackVar cattr(ctx, b, 0x10);
        write_name(b, cattr.addr());
        if (const s32 r = lv2_call(ctx, kLwcondCreate, {jq + jq_off::lwcond, jq + jq_off::lwmutex0, cattr.addr()});
            r != lv2::CELL_OK)
            return r;
    }
    ps_write32(b, jq + jq_off::spurs, spurs);
    ps_write32(b, jq + jq_off::sdk_version, sdk_version);
    ps_write32(b, jq + jq_off::jq_revision, jq_revision);
    ps_write32(b, jq + jq_off::max_size, max_size);
    u8 max_jobs = 0xFF; // SDK 3.1 and before: no grab parameters
    if (sdk_version > 0x30FFFF) {
        ps_write8(b, jq + jq_off::memory_check, ps_read8(b, attr + qattr::isJobTypeMemoryCheck) ? 2 : 0);
        max_jobs = ps_read8(b, attr + qattr::maxNumJobsOnASpu + 3);
    } else {
        ps_write8(b, jq + jq_off::memory_check, 0);
    }
    u64 control = ps_read64(b, jq + jq_off::control);
    control = (control & ~0x4000ull) | u64{ps_read8(b, attr + qattr::doBusyWaiting) & 1u} << 14;
    control = (control & ~0x1000ull) | u64{ps_read8(b, attr + qattr::isHaltOnError) & 1u} << 12;
    ps_write64(b, jq + jq_off::control, control);
    ps_write8(b, jq + jq_off::max_grab, static_cast<u8>(ps_read32(b, attr + qattr::maxGrab)));
    ps_write8(b, jq + jq_off::submit_with_entry_lock, ps_read8(b, attr + qattr::submitWithEntryLock));
    ps_write64(b, jq + jq_off::header, ps_read64(b, jq + jq_off::header) | 0x60);
    ps_write16(b, jq + jq_off::wait_depth, 0x100);
    ps_write8(b, jq + jq_off::max_jobs_on_spu, max_jobs);
    ps_write32(b, jq + jq_off::revision, jq_revision);
    ps_write32(b, jq + jq_off::command_list, list);
    ps_write16(b, jq + jq_off::depth, static_cast<u16>(depth));
    ps_write8(b, jq + jq_off::wait_header + 5, 1);
    ps_write32(b, jq + jq_off::one_1c4, 1);
    ps_write8(b, jq + jq_off::wait_header + 6, 1);

    if (pool) {
        // Eight classes from the largest, each a free list linked through the descriptors' first word, then a 1 KB
        // waiting ring for each class that has descriptors.
        u32 counts[8];
        for (u32 i = 0; i < 8; i++) counts[i] = ps_read32(b, pool_desc + 4 * i);
        u64 total = 0;
        for (u32 i = 0; i < 8; i++) total += u64{counts[i]} * kClassSize[i] + (counts[i] ? 0x400 : 0);
        if (!mapped(ctx, pool, total, fn, "descriptor pool")) return err(CELL_SPURS_JOB_ERROR_INVAL);
        u32 at = pool;
        for (u32 i = 8; i-- > 0;) {
            const u32 n = counts[i];
            if (!n) continue;
            const u32 size = kClassSize[i];
            const u32 rec = jq + jq_off::pool_class + 16 * i;
            ps_write32(b, rec, at);
            ps_write8(b, rec + 0xD, 1);
            ps_write16(b, rec + 4, static_cast<u16>(n));
            ps_write8(b, rec + 0xE, 1);
            ps_write32(b, jq + jq_off::pool_start + 4 * i, at);
            const u32 start = at;
            at += n * size;
            ps_write32(b, jq + jq_off::pool_end + 4 * i, at);
            for (u32 k = 1; k < n; k++) ps_write32(b, start + (k - 1) * size, start + k * size);
            ps_write32(b, start + (n - 1) * size, 0);
        }
        ps_write32(b, jq + jq_off::pool_base, pool);
        ps_write32(b, jq + jq_off::pool_limit, at);
        const u32 rings = at;
        u32 bytes = 0;
        for (u32 i = 0; i < 8; i++) {
            if (!counts[i]) continue;
            ps_write32(b, jq + jq_off::pool_wait + 4 * i, at);
            bytes += 0x400;
            at += 0x400;
        }
        if (bytes) std::memset(b + rings, 0, bytes);
    }

    // The workload: libspurs_jq's policy module (its first 16 KB loaded by the kernel, the rest by itself: the size it
    // needs, rounded up to 128 bytes, in the argument's high word, the queue in the low one).
    u32 pm_size = 0;
    const u32 pm = policy_module(ctx, pm_size);
    if (!pm) {
        log_call_site(ctx, b, std::format("{}: no job queue policy module (libspurs_jq's SPU half comes from the "
                                          "installed system software); CELL_SPURS_JOB_ERROR_STAT", fn));
        return undo_create(ctx, b, jq, err(CELL_SPURS_JOB_ERROR_STAT));
    }
    StackVar wattr(ctx, b, 0x200);
    const u64 data = (u64{(pm_size + 127) & 0xFFFFFF80u} << 32) | jq;
    s32 r = spurs_call(ctx, kWorkloadAttributeInitialize, {wattr.addr(), 1, kLibrarySdkVersion, pm,
                                                               std::min<u32>(pm_size, 0x4000), data, priorities, 1, max_contention});
    if (r != lv2::CELL_OK) return undo_create(ctx, b, jq, r);
    spurs_call(ctx, kWorkloadAttributeSetName, {wattr.addr(), string_of(kStringJobQueue), name});
    StackVar wid(ctx, b, 4);
    r = spurs_call(ctx, kAddWorkloadWithAttribute, {spurs, wid.addr(), wattr.addr()});
    if (r != lv2::CELL_OK) return undo_create(ctx, b, jq, r);
    const u32 id = ps_read32(b, wid.addr());
    ps_write32(b, jq + jq_off::spurs_wid, spurs | id);
    // libspurs_jq returns here with what it made in place (no clean-up).
    r = spurs_call(ctx, kSetExceptionEventHandler, {spurs, id, exception_handler_descriptor(), jq});
    if (r != lv2::CELL_OK) return r;
    {
        // The title's PARAM.SFO: libspurs_jq looks its ID up in its list of titles with workarounds (not reproduced).
        StackVar sfo(ctx, b, 0x40);
        lv2_call(ctx, kGetParamsfo, {sfo.addr()});
    }
    lwsync();
    st64(b, jq + jq_off::control, ps_read64(b, jq + jq_off::control) | 0x800);
    lwsync();
    if (ps_read8(b, attr + qattr::doBusyWaiting)) {
        spurs_call(ctx, kSendWorkloadSignal, {spurs, id});
        spurs_call(ctx, kWakeUp, {spurs});
    }
    return r;
}

s32 _cellSpursCreateJobQueue(PPUContext& ctx, u32 jq_revision, u32 sdk_version, u32 spurs, u32 jq, u32 attr, u32 name,
    u32 list, u32 depth, u32 max_contention, u32 priorities)
{
    return _cellSpursCreateJobQueueWithJobDescriptorPool(ctx, jq_revision, sdk_version, spurs, jq, attr, 0, 0, name, list,
        depth, max_contention, priorities);
}

s32 cellSpursJoinJobQueue(PPUContext& ctx, u32 jq, u32 exit_code)
{
    if (!jq || !exit_code) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (jq & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!mapped(ctx, jq, jq_off::size, "cellSpursJoinJobQueue", "job queue")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 wid = ps_read32(b, jq + jq_off::spurs_wid) & 0x7F;
    if (wid > 31) return err(CELL_SPURS_JOB_ERROR_INVAL);
    s32 r = spurs_call(ctx, kWaitForWorkloadShutdown, {ps_read32(b, jq + jq_off::spurs), wid});
    if (r != lv2::CELL_OK) {
        if (static_cast<u32>(r) == CELL_SPURS_POLICY_MODULE_ERROR_INVAL) return err(CELL_SPURS_JOB_ERROR_INVAL);
        if (static_cast<u32>(r) == CELL_SPURS_POLICY_MODULE_ERROR_STAT) return err(CELL_SPURS_JOB_ERROR_STAT);
        return r;
    }
    r = spurs_call(ctx, kRemoveWorkload, {ps_read32(b, jq + jq_off::spurs), wid});
    if (r != lv2::CELL_OK) return r;
    sc(ctx, 83, {ps_read32(b, jq + jq_off::event_flag0)});
    sc(ctx, 83, {ps_read32(b, jq + jq_off::event_flag1)});
    lv2_call(ctx, kLwmutexDestroy, {jq + jq_off::lwmutex0});
    lv2_call(ctx, kLwmutexDestroy, {jq + jq_off::lwmutex1});
    lv2_call(ctx, kLwcondDestroy, {jq + jq_off::lwcond});
    if (!mapped(ctx, exit_code, 4, "cellSpursJoinJobQueue", "exit code")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    ps_write32(b, exit_code, ps_read32(b, jq + jq_off::exit_code));
    lwsync();
    st32(b, jq + jq_off::spurs_wid, ps_read32(b, jq + jq_off::spurs) | 0x20);
    return lv2::CELL_OK;
}

s32 cellSpursShutdownJobQueue(PPUContext& ctx, u32 jq)
{
    if (!jq) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (jq & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!mapped(ctx, jq, jq_off::size, "cellSpursShutdownJobQueue", "job queue")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 wid = ps_read32(b, jq + jq_off::spurs_wid) & 0x7F;
    if (wid > 31) return err(CELL_SPURS_JOB_ERROR_INVAL);
    for (Reserve r;;) {
        const u64 v = r.ldarx(b, jq + jq_off::control);
        lwsync();
        if (ps_read16(b, jq + jq_off::handles)) return err(CELL_SPURS_JOB_ERROR_BUSY);
        if (!(v & 0x800)) return err(CELL_SPURS_JOB_ERROR_STAT);
        if (r.stdcx(b, v & ~0x800ull)) break;
    }
    const s32 r = spurs_call(ctx, kShutdownWorkload, {ps_read32(b, jq + jq_off::spurs), wid});
    if (static_cast<u32>(r) == CELL_SPURS_POLICY_MODULE_ERROR_STAT) return err(CELL_SPURS_JOB_ERROR_STAT);
    return r;
}

// cellSpursJobQueueSetWaitingMode(jobQueue, mode): 1 busy waiting (the control word's bit 0x4000), 0 not. Switching it
// on while the policy module's byte is 0 kicks the workload.
s32 cellSpursJobQueueSetWaitingMode(PPUContext& ctx, u32 jq, u32 mode)
{
    if (!jq) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (jq & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (mode > 1) return err(CELL_SPURS_JOB_ERROR_INVAL);
    const char* fn = "cellSpursJobQueueSetWaitingMode";
    if (!mapped(ctx, jq, jq_off::size, fn, "job queue")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 spurs = ps_read32(b, jq + jq_off::spurs);
    if (!mapped(ctx, spurs + 0xD6C, 4, fn, "CellSpurs")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (ps_read32(b, spurs + 0xD6C)) return err(CELL_SPURS_JOB_ERROR_STAT); // the instance's exception state
    bool kick_it = false;
    for (Reserve r;;) {
        const u64 v = r.ldarx(b, jq + jq_off::control);
        u64 on = 0;
        kick_it = false;
        if (!(v & 0x4000)) {
            if (mode == 1) {
                on = 1;
                kick_it = (v >> 56) == 0;
            }
        } else if (mode == 1) {
            on = 1;
        }
        if (r.stdcx(b, (v & ~0x4000ull) | on << 14)) break;
    }
    if (kick_it && static_cast<u32>(kick(ctx, b, jq)) == CELL_SPURS_POLICY_MODULE_ERROR_STAT)
        return err(CELL_SPURS_JOB_ERROR_STAT);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------------ getters

u32 cellSpursJobQueueGetSpurs(PPUContext& ctx, u32 jq)
{
    if (!jq || (jq & 0x7F)) return 0;
    if (!mapped(ctx, jq, jq_off::size, "cellSpursJobQueueGetSpurs", "job queue")) return 0;
    return ps_read32(gb(ctx), jq + jq_off::spurs);
}

s32 cellSpursJobQueueGetHandleCount(PPUContext& ctx, u32 jq)
{
    if (!jq) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (jq & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!mapped(ctx, jq, jq_off::size, "cellSpursJobQueueGetHandleCount", "job queue")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    return ps_read16(gb(ctx), jq + jq_off::handles);
}

// cellSpursJobQueueGetError(jobQueue, int* exitCode, void** cause): the error the policy module recorded, and with one
// its cause (the low word of the doubleword at 0xF08).
s32 cellSpursJobQueueGetError(PPUContext& ctx, u32 jq, u32 exit_code, u32 cause)
{
    if (!jq || !cause || !exit_code) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (jq & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    const char* fn = "cellSpursJobQueueGetError";
    if (!mapped(ctx, jq, jq_off::size, fn, "job queue") || !mapped(ctx, exit_code, 4, fn, "exit code") ||
        !mapped(ctx, cause, 4, fn, "cause"))
        return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    ps_write32(b, exit_code, ps_read32(b, jq + jq_off::exit_code));
    const u32 code = ps_read32(b, jq + jq_off::exit_code);
    ps_write32(b, cause, code ? static_cast<u32>(ps_read64(b, jq + jq_off::cause)) : 0);
    return lv2::CELL_OK;
}

s32 cellSpursJobQueueGetMaxSizeJobDescriptor(PPUContext& ctx, u32 jq)
{
    if (!jq) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (jq & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!mapped(ctx, jq, jq_off::size, "cellSpursJobQueueGetMaxSizeJobDescriptor", "job queue"))
        return err(CELL_SPURS_JOB_ERROR_INVAL);
    return static_cast<s32>(ps_read32(gb(ctx), jq + jq_off::max_size));
}

s32 cellSpursGetJobQueueId(PPUContext& ctx, u32 jq, u32 id)
{
    if (!jq || !id) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (jq & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    const char* fn = "cellSpursGetJobQueueId";
    if (!mapped(ctx, jq, jq_off::size, fn, "job queue") || !mapped(ctx, id, 4, fn, "output"))
        return err(CELL_SPURS_JOB_ERROR_INVAL);
    const u32 wid = ps_read32(gb(ctx), jq + jq_off::spurs_wid) & 0x7F;
    if (wid > 31) return err(CELL_SPURS_JOB_ERROR_STAT);
    ps_write32(gb(ctx), id, wid);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------ the waiting rings

// eventflag_reserveBit: the first free one of the queue's 128 event flag bits, taken.
u32 reserve_bit(u8* b, u32 jq)
{
    for (u32 w = 0; w <= 3; w++) {
        const u32 at = jq + jq_off::wait_bits + 4 * w;
        for (Reserve r;;) {
            const u32 word = r.lwarx(b, at);
            const u32 z = static_cast<u32>(std::countl_zero(~word));
            if (z > 31) break;
            if (r.stwcx(b, word | (0x80000000u >> z))) return z + 32 * w;
        }
    }
    jq_assert("job_queue_internal.h", 51, "eventflag_reserveBit");
}

// eventflag_releaseBit.
void release_bit(u8* b, u32 jq, u32 bit)
{
    const u32 at = jq + jq_off::wait_bits + 4 * (bit / 32);
    for (Reserve r;;) {
        const u32 word = r.lwarx(b, at);
        if (r.stwcx(b, word & ~(0x80000000u >> (bit % 32)))) return;
    }
}

// A waiting ring entry is valid in a lap when its bit 0 is the lap's: the ring's header holds the write index (bits
// 47..32), the pushers in flight (byte 4) and the lap (byte 6). A waiter counts itself in, takes the first entry of the
// lap after the write index that none before it took (within the pushers in flight, every entry before it taken),
// writes it with the lap's bit, and moves the write index on (the lap turning at the end). Returns whether the entry's
// earlier value had bit 1 clear: no wake-up was left there before the waiter came, so it sleeps (libspurs_jq's
// tryToWait).
bool wait_ring_push(u8* b, u32 hdr, u32 entries, u16 ring_depth, u64 value)
{
    const u32 depth = ring_depth;
    u32 W = 0;
    for (Reserve r;;) {
        const u64 h = r.ldarx(b, hdr);
        const u32 pushers = (h >> 24) & 0xFF;
        if (pushers > 0x7E) jq_assert("job_queue_push.cpp", 115, "tryToWait");
        const u64 n = (h & ~(0xFFull << 24)) | (((h >> 24) + 1) & 0xFF) << 24;
        W = static_cast<u32>(n >> 32) & 0xFFFF;
        if (r.stdcx(b, n)) break;
    }
    u32 k = 0;
    for (;;) {
        u32 slot = W + k;
        if (slot >= depth) slot -= depth;
        const u32 ea = entries + slot * 8;
        Reserve re;
        const u64 e = re.ldarx(b, ea);
        lwsync();
        const u64 h = ps_read64(b, hdr);
        const u32 W2 = static_cast<u32>(h >> 32) & 0xFFFF;
        const u32 phase = (h >> 8) & 0xFF, notphase = phase == 0 ? 1 : 0;
        const u32 pushers = (h >> 24) & 0xFF;
        u32 exp = phase, pos = slot;
        if (slot < W2) {
            exp = notphase;
            pos = slot + depth;
        }
        const u32 add = (pos == W2 && phase != exp) ? depth : 0;
        const u32 d = add + (pos - W2);
        bool ok = false;
        if (pushers > d) {
            // Every entry from the write index to this one taken in its lap.
            ok = true;
            for (u32 j = 0;; j++) {
                const u32 p0 = slot >= W2 ? slot : slot + depth;
                const u32 dd = (p0 == W2) ? (phase == exp ? 0 : depth) : p0 - W2;
                if (dd <= j) break;
                u32 p = W2 + j;
                if (p >= depth) p -= depth;
                const u32 pexp = p < W2 ? notphase : phase;
                if ((ps_read64(b, entries + p * 8) & 1) != pexp) {
                    ok = false;
                    break;
                }
            }
        }
        if (!ok) {
            k = 0;
            W = W2;
            continue;
        }
        if ((e & 1) == exp) { // taken in this lap already: the next one
            k = W == W2 ? k + 1 : 0;
            W = W2;
            continue;
        }
        if (!re.stdcx(b, exp | value)) {
            W = W2;
            continue;
        }
        for (Reserve r;;) {
            const u64 h3 = r.ldarx(b, hdr);
            u64 n = (h3 & ~(0xFFull << 24)) | (((h3 >> 24) - 1) & 0xFF) << 24;
            const u32 next = static_cast<u32>((h3 >> 32) + 1) & 0xFFFF;
            n = (n & ~(0xFFFFull << 32)) | u64{next} << 32;
            if (next == depth) {
                const u64 flipped = ((n >> 8) & 0xFF) == 0 ? 1 : 0;
                n = (n & ~(0xFFull << 8)) | flipped << 8;
                n &= ~(0xFFFFull << 32);
            }
            if (r.stdcx(b, n)) break;
        }
        return !((e >> 1) & 1);
    }
}

// Waits for room as libspurs_jq does when a ring or a pool class is full: a fiber waits for a signal, a PPU thread on an
// event flag bit, each through an entry in the waiting ring at `hdr` (the policy module wakes the waiters as it frees
// entries). A wake-up that came first leaves the entry marked and nobody sleeps.
void wait_for_room(PPUContext& ctx, u8* b, u32 jq, u32 hdr, u32 entries, u16 ring_depth)
{
    const u32 fiber = fiber_self(ctx);
    u64 value = u64{fiber} << 32;
    u32 bit = 0;
    if (!fiber) {
        bit = reserve_bit(b, jq);
        value = u64{bit} << 32 | kWaitThread;
    }
    const bool sleep = wait_ring_push(b, hdr, entries, ring_depth, value);
    const bool thread = (value & kWaitThread) != 0;
    if (sleep) {
        if (thread) {
            // sys_event_flag_wait(id, bit, AND | CLEAR, NULL, no timeout); the bit is kept when the wait fails.
            const u32 id = ps_read32(b, jq + jq_off::event_flag0 + 4 * (bit / 64));
            if (sc(ctx, 85, {id, 1ull << (bit % 64), 0x11, 0, 0}) == lv2::CELL_OK) release_bit(b, jq, bit);
        } else {
            fiber_wait_signal(ctx);
        }
    } else if (thread) {
        release_bit(b, jq, bit);
    }
}

// --------------------------------------------------------------------------------------------- the command ring

// Pushes `command` for `handle` (an open handle of the queue) onto the command ring; `blocking` waits for room, else a
// full ring is AGAIN. Kicks the workload unless it busy-waits (or its policy module's byte says it is on an SPU) and the
// ring was not empty (libspurs_jq's _cellSpursJobQueuePushBodyInternal).
s32 push_command(PPUContext& ctx, u32 jq, u32 handle, u64 command, bool blocking)
{
    if (handle > 1023) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    if (!((ps_read32(b, jq + jq_off::handle_bits + 4 * (handle / 32)) >> (31 - handle % 32)) & 1))
        return err(CELL_SPURS_JOB_ERROR_INVAL);
    const u32 list = ps_read32(b, jq + jq_off::command_list);
    const u32 depth = ps_read16(b, jq + jq_off::depth);
    const auto byte = [](u64 v, u32 i) { return static_cast<u32>(v >> (56 - 8 * i)) & 0xFF; };
    const auto set_byte = [](u64 v, u32 i, u32 x) {
        const u32 sh = 56 - 8 * i;
        return (v & ~(0xFFull << sh)) | u64{x & 0xFF} << sh;
    };
    const auto wait = [&] {
        wait_for_room(ctx, b, jq, jq + jq_off::wait_header, list + ((ps_read16(b, jq + jq_off::depth) << 3) & 0x7FFF8),
            ps_read16(b, jq + jq_off::wait_depth));
    };
    const auto kick_if = [&](bool was_empty) {
        const u64 control = ps_read64(b, jq + jq_off::control);
        if (((control & 0x4000) || (control >> 56)) && !was_empty) return;
        kick(ctx, b, jq);
    };

    if (ps_read8(b, jq + jq_off::submit_with_entry_lock)) {
        // The entry is the header's own: one store takes the write index and moves it on.
        bool waited = false, was_empty = false;
        u32 slot = 0, lap = 0;
        for (;;) {
            Reserve r;
            u64 n = r.ldarx(b, jq + jq_off::header);
            const u32 dec = waited ? 1 : 0;
            if (waited) n = set_byte(n, 6, byte(n, 6) - 1);
            n = set_byte(n, 5, byte(n, 5) - dec);
            if (byte(n, 6) == 127) return err(CELL_SPURS_JOB_ERROR_AGAIN);
            const u32 W = static_cast<u32>(n >> 32) & 0xFFFF, R = static_cast<u32>(n >> 48);
            bool full = false;
            if (W == R && ((n >> 6) & 1) != ((n >> 5) & 1)) {
                if (!blocking) return err(CELL_SPURS_JOB_ERROR_AGAIN);
                n = set_byte(n, 6, byte(n, 6) + 1);
                full = true;
            } else {
                slot = W;
                was_empty = W == R;
                lap = (n >> 5) & 1;
                const u32 next = (W + 1) & 0xFFFF;
                n = (n & ~(0xFFFFull << 32)) | u64{next} << 32;
                if (next == depth) n = (n ^ 0x20) & ~(0xFFFFull << 32);
            }
            if (!r.stdcx(b, n)) continue;
            if (!full) break;
            wait();
            waited = true;
        }
        st64(b, list + ((slot << 3) & 0x7FFF8), command | lap);
        lwsync();
        kick_if(was_empty);
        return lv2::CELL_OK;
    }

    // Pushers in flight count themselves in the header's byte 4 (with the waiters, at most 127); the slot after them is
    // the one this push may need, full when it is the read index in the other lap.
    bool waited = false;
    u32 W = 0;
    for (;;) {
        Reserve r;
        u64 n = r.ldarx(b, jq + jq_off::header);
        const u32 dec = waited ? 1 : 0;
        if (waited) n = set_byte(n, 6, byte(n, 6) - 1);
        n = set_byte(n, 5, byte(n, 5) - dec);
        const u32 pushers = byte(n, 4), waiters = byte(n, 6);
        if (pushers + waiters == 127) return err(CELL_SPURS_JOB_ERROR_AGAIN);
        u32 pos = pushers + (static_cast<u32>(n >> 32) & 0xFFFF);
        u32 lap = (n >> 5) & 1;
        if (pos >= depth) {
            pos -= depth;
            lap ^= 1;
        }
        bool full = false;
        if ((n >> 48) == pos && ((n >> 6) & 1) != lap) {
            if (!blocking) return err(CELL_SPURS_JOB_ERROR_AGAIN);
            n = set_byte(n, 6, waiters + 1);
            full = true;
        } else {
            n = set_byte(n, 4, pushers + 1);
        }
        W = static_cast<u32>(n >> 32) & 0xFFFF;
        if (!r.stdcx(b, n)) continue;
        if (!full) break;
        wait();
        waited = true;
    }
    // Take the first entry of the lap after the write index that the pushers before this one left.
    u32 k = 0;
    for (;;) {
        u32 slot = W + k;
        if (slot >= depth) slot -= depth;
        const u32 ea = list + slot * 8;
        Reserve re;
        const u64 e = re.ldarx(b, ea);
        lwsync();
        const u64 h = ps_read64(b, jq + jq_off::header);
        const u32 phase = (h >> 5) & 1, notphase = phase ^ 1;
        const u32 W2 = static_cast<u32>(h >> 32) & 0xFFFF;
        const u32 pushers = (h >> 24) & 0xFF;
        u32 exp = phase, pos = slot;
        if (slot < W2) {
            exp = notphase;
            pos = slot + depth;
        }
        const u32 add = (pos == W2 && phase != exp) ? depth : 0;
        const u32 d = add + (pos - W2);
        bool ok = false;
        if (pushers > d) {
            ok = true;
            for (u32 j = 0;; j++) {
                const u32 p0 = slot >= W2 ? slot : slot + depth;
                const u32 dd = (p0 == W2) ? (phase == exp ? 0 : depth) : p0 - W2;
                if (dd <= j) break;
                u32 p = W2 + j;
                if (p >= depth) p -= depth;
                const u32 pexp = p < W2 ? notphase : phase;
                if ((ps_read64(b, list + p * 8) & 1) != pexp) {
                    ok = false;
                    break;
                }
            }
        }
        if (!ok) {
            k = 0;
            W = W2;
            continue;
        }
        if ((e & 1) == exp) {
            k = W == W2 ? k + 1 : 0;
            W = W2;
            continue;
        }
        if (!re.stdcx(b, command | exp)) {
            W = W2;
            continue;
        }
        break;
    }
    // Count this push out and move the write index on (the lap turning at the end); kick as the header says.
    for (;;) {
        Reserve r;
        const u64 h = r.ldarx(b, jq + jq_off::header);
        u64 n = set_byte(h, 4, byte(h, 4) - 1);
        const u32 Wh = static_cast<u32>(h >> 32) & 0xFFFF, Rh = static_cast<u32>(h >> 48);
        const u32 next = (Wh + 1) & 0xFFFF;
        n = (n & ~(0xFFFFull << 32)) | u64{next} << 32;
        if (next == depth) n = ((n & ~0x20ull) | (((h >> 5) & 1) ^ 1) << 5) & ~(0xFFFFull << 32);
        lwsync();
        const u64 control = ps_read64(b, jq + jq_off::control);
        bool signal = true;
        if ((control & 0x4000) || (control >> 56)) signal = Wh == Rh;
        if (!r.stdcx(b, n)) continue;
        if (signal) kick(ctx, b, jq);
        return lv2::CELL_OK;
    }
}

// A job descriptor's size: 64, or a multiple of 128 up to 1023.
bool bad_size(u32 size) { return size != 64 && ((size & 0x7F) || size > 1023); }
// Its size in the command (size / 128 in bits 3..1; 0 for 64 bytes).
u64 size_bits(u32 size) { return (size >> 6) & 0x03FFFFFEu; }

// The pool's descriptors: [pool start, descriptors' end).
bool in_pool(u8* b, u32 jq, u32 desc)
{
    return ps_read32(b, jq + jq_off::pool_base) <= desc && desc < ps_read32(b, jq + jq_off::pool_limit);
}

bool queue_mapped(PPUContext& ctx, u32 jq, std::string_view fn) { return mapped(ctx, jq, jq_off::size, fn, "job queue"); }

// _cellSpursJobQueuePushJobListBody(jobQueue, handle, jobList, tag, semaphore, isBlocking)
s32 _cellSpursJobQueuePushJobListBody(PPUContext& ctx, u32 jq, s32 handle, u32 job_list, u32 tag, u32 sem, u8 blocking)
{
    if (!jq || !job_list) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if ((jq & 0x7F) || (job_list & 0xF) || (sem & 0x7F)) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    const char* fn = "_cellSpursJobQueuePushJobListBody";
    if (!queue_mapped(ctx, jq, fn) || !mapped(ctx, job_list, 8, fn, "job list")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 size = ps_read32(b, job_list + 4);
    if (bad_size(size) || tag > 15 || ps_read32(b, jq + jq_off::max_size) < size) return err(CELL_SPURS_JOB_ERROR_INVAL);
    lwsync();
    return push_command(ctx, jq, static_cast<u32>(handle), u64{tag} << 35 | job_list | u64{sem} << 32 | kCmdJobList,
        blocking != 0);
}

// _cellSpursJobQueuePushJobBody2(jobQueue, handle, job, size, tag, semaphore, exclusive, isBlocking)
s32 _cellSpursJobQueuePushJobBody2(PPUContext& ctx, u32 jq, s32 handle, u32 job, u32 size, u32 tag, u32 sem, u8 flag,
    u8 blocking)
{
    if (!jq || !job) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if ((jq & 0x7F) || (job & 0xF) || (sem & 0x7F)) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (bad_size(size) || tag > 15) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!queue_mapped(ctx, jq, "_cellSpursJobQueuePushJobBody2")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (ps_read32(gb(ctx), jq + jq_off::max_size) < size) return err(CELL_SPURS_JOB_ERROR_INVAL);
    lwsync();
    return push_command(ctx, jq, static_cast<u32>(handle),
        u64{sem} << 32 | u64{tag} << 35 | job | u64{flag != 0} << 34 | size_bits(size), blocking != 0);
}

// _cellSpursJobQueuePushJob2Body(jobQueue, handle, job, size, tag, flags, semaphore): flags bit 2 = do not wait, bit 1
// the job's second flag; descriptors of the pool are refused.
s32 _cellSpursJobQueuePushJob2Body(PPUContext& ctx, u32 jq, s32 handle, u32 job, u32 size, u32 tag, u32 flags, u32 sem)
{
    if (flags & ~7u) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!jq || !job) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if ((jq & 0x7F) || (job & 0xF) || (sem & 0x7F)) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (bad_size(size) || tag > 15) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!queue_mapped(ctx, jq, "_cellSpursJobQueuePushJob2Body")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    if (ps_read32(b, jq + jq_off::max_size) < size || in_pool(b, jq, job)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    lwsync();
    return push_command(ctx, jq, static_cast<u32>(handle),
        u64{sem} << 32 | u64{tag} << 35 | u64{(flags >> 1) & 1} << 34 | size_bits(size) | job, !(flags & 4));
}

// _cellSpursJobQueuePushAndReleaseJobBody(jobQueue, handle, job, size, tag, flags, semaphore): a descriptor of the pool,
// which the policy module gives back after the job ran.
s32 _cellSpursJobQueuePushAndReleaseJobBody(PPUContext& ctx, u32 jq, s32 handle, u32 job, u32 size, u32 tag, u32 flags,
    u32 sem)
{
    if (flags & ~7u) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!jq || !job) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if ((jq & 0x7F) || (job & 0xF) || (sem & 0x7F)) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (bad_size(size) || tag > 15) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!queue_mapped(ctx, jq, "_cellSpursJobQueuePushAndReleaseJobBody")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    if (ps_read32(b, jq + jq_off::max_size) < size || !in_pool(b, jq, job)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    lwsync();
    return push_command(ctx, jq, static_cast<u32>(handle),
        u64{sem} << 32 | u64{tag} << 35 | job | kCmdRelease | u64{(flags >> 1) & 1} << 34 | size_bits(size),
        !(flags & 4));
}

// _cellSpursJobQueuePushJobBody(jobQueue, handle, job, size, tag, semaphore, isBlocking)
s32 _cellSpursJobQueuePushJobBody(PPUContext& ctx, u32 jq, s32 handle, u32 job, u32 size, u32 tag, u32 sem, u8 blocking)
{
    if (!jq || !job) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if ((jq & 0x7F) || (job & 0xF) || (sem & 0x7F)) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (bad_size(size) || tag > 15) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!queue_mapped(ctx, jq, "_cellSpursJobQueuePushJobBody")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (ps_read32(gb(ctx), jq + jq_off::max_size) < size) return err(CELL_SPURS_JOB_ERROR_INVAL);
    lwsync();
    return push_command(ctx, jq, static_cast<u32>(handle), u64{tag} << 35 | job | u64{sem} << 32 | size_bits(size),
        blocking != 0);
}

// _cellSpursJobQueuePushBody(jobQueue, handle, job, size, semaphore, isBlocking): tag 0.
s32 _cellSpursJobQueuePushBody(PPUContext& ctx, u32 jq, s32 handle, u32 job, u32 size, u32 sem, u8 blocking)
{
    if (!jq || !job) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if ((jq & 0x7F) || (job & 0xF) || (sem & 0x7F)) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (bad_size(size)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!queue_mapped(ctx, jq, "_cellSpursJobQueuePushBody")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (ps_read32(gb(ctx), jq + jq_off::max_size) < size) return err(CELL_SPURS_JOB_ERROR_INVAL);
    lwsync();
    return push_command(ctx, jq, static_cast<u32>(handle), u64{sem} << 32 | job | size_bits(size), blocking != 0);
}

// _cellSpursJobQueuePushSync(jobQueue, handle, tagMask, isBlocking): SYNC on the tags in the mask (1..0xFFFF).
s32 _cellSpursJobQueuePushSync(PPUContext& ctx, u32 jq, s32 handle, u32 mask, u8 blocking)
{
    if (!jq) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (jq & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!mask || (mask & 0xFFFF0000u)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!queue_mapped(ctx, jq, "_cellSpursJobQueuePushSync")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    return push_command(ctx, jq, static_cast<u32>(handle), u64{mask} << 48 | kCmdSync, blocking != 0);
}

s32 _cellSpursJobQueuePushFlush(PPUContext& ctx, u32 jq, s32 handle, u8 blocking)
{
    if (!jq) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (jq & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!queue_mapped(ctx, jq, "_cellSpursJobQueuePushFlush")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    lwsync();
    return push_command(ctx, jq, static_cast<u32>(handle), kCmdFlush, blocking != 0);
}

// ----------------------------------------------------------------------------------------- the descriptor pool

// A descriptor of `size` from the pool's class for it (64 bytes: class 0, else size / 128), waiting for one when
// `blocking` (libspurs_jq's allocation through a handle).
s32 allocate_descriptor(PPUContext& ctx, u32 jq, u32 handle, u32 size, bool blocking, u32 out)
{
    if (handle > 1023) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    if (!((ps_read32(b, jq + jq_off::handle_bits + 4 * (handle / 32)) >> (31 - handle % 32)) & 1))
        return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (size != 64 && ((size & 0x7F) || size <= 127 || size > 1023)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!out) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    const u32 c = size >> 7;
    if (!ps_read32(b, jq + jq_off::pool_start + 4 * c)) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (!mapped(ctx, out, 4, "_cellSpursJobQueueAllocateJobDescriptorBody", "output")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    const u32 rec = jq + jq_off::pool_class + 16 * c;
    bool waited = false;
    for (;;) {
        Reserve r;
        u64 f = r.ldarx(b, rec);
        u32 dec = 0;
        if (waited) {
            f = (f & ~0xFFull) | ((f - 1) & 0xFF);
            dec = 1;
        }
        f = (f & ~0xFF00ull) | u64{(static_cast<u32>(f >> 8) - dec) & 0xFF} << 8;
        if ((f & 0xFF) == 0x7F) return err(CELL_SPURS_JOB_ERROR_AGAIN);
        u32 head = 0;
        if (((f >> 16) & 0xFFFF) == 0) {
            if (!blocking) return err(CELL_SPURS_JOB_ERROR_AGAIN);
            f = (f & ~0xFFull) | ((f + 1) & 0xFF);
        } else {
            head = static_cast<u32>(f >> 32);
            const u32 next = ps_read32(b, head);
            f = (f & 0xFFFFFFFFull) | u64{next} << 32;
            f = (f & ~(0xFFFFull << 16)) | ((((f >> 16) & 0xFFFF) - 1) & 0xFFFF) << 16;
        }
        lwsync();
        if (!r.stdcx(b, f)) continue;
        if (head) {
            ps_write32(b, out, head);
            return lv2::CELL_OK;
        }
        wait_for_room(ctx, b, jq, rec + 8, ps_read32(b, jq + jq_off::pool_wait + 4 * c), 0x80);
        waited = true;
    }
}

// _cellSpursJobQueueAllocateJobDescriptorBody(jobQueue, handle, size, flags, job*): flags 0, or 4 = do not wait (libspurs_jq
// tests flags & ~4 with a rotate pair).
s32 _cellSpursJobQueueAllocateJobDescriptorBody(PPUContext& ctx, u32 jq, s32 handle, u32 size, u32 flags, u32 out)
{
    if (!jq || !out) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (jq & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (flags & ~4u) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!queue_mapped(ctx, jq, "_cellSpursJobQueueAllocateJobDescriptorBody")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    return allocate_descriptor(ctx, jq, static_cast<u32>(handle), size, !(flags & 4), out);
}

// ------------------------------------------------------------------------------------------------ handles

s32 cellSpursJobQueueOpen(PPUContext& ctx, u32 jq, u32 handle)
{
    if (!jq || !handle) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (jq & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    const char* fn = "cellSpursJobQueueOpen";
    if (!queue_mapped(ctx, jq, fn) || !mapped(ctx, handle, 4, fn, "handle")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    // The count first (at most 1024, the queue running), then a free bit; a full bitmap leaves the count up.
    for (Reserve r;;) {
        const u32 w = r.lwarx(b, jq + jq_off::handles);
        lwsync();
        const u64 control = ps_read64(b, jq + jq_off::control);
        const u32 count = w >> 16;
        if (!(control & 0x800)) return err(CELL_SPURS_JOB_ERROR_STAT);
        if (count == 1024) return err(CELL_SPURS_JOB_ERROR_AGAIN);
        if (r.stwcx(b, (w & 0xFFFF) | (count + 1) << 16)) break;
    }
    for (u32 w = 0; w <= 31; w++) {
        const u32 at = jq + jq_off::handle_bits + 4 * w;
        for (Reserve r;;) {
            const u32 word = r.lwarx(b, at);
            const u32 z = static_cast<u32>(std::countl_zero(~word));
            if (z > 31) break;
            if (r.stwcx(b, word | (0x80000000u >> z))) {
                ps_write32(b, handle, 32 * w + z);
                return lv2::CELL_OK;
            }
        }
    }
    return err(CELL_SPURS_JOB_ERROR_AGAIN);
}

s32 cellSpursJobQueueClose(PPUContext& ctx, u32 jq, s32 handle)
{
    if (!jq) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (jq & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    const u32 h = static_cast<u32>(handle);
    if (h > 1023) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!queue_mapped(ctx, jq, "cellSpursJobQueueClose")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 at = jq + jq_off::handle_bits + 4 * (h / 32), bit = 0x80000000u >> (h % 32);
    for (Reserve r;;) {
        const u32 word = r.lwarx(b, at);
        if (!(word & bit)) return err(CELL_SPURS_JOB_ERROR_INVAL);
        if (r.stwcx(b, word & ~bit)) break;
    }
    for (Reserve r;;) {
        const u32 w = r.lwarx(b, jq + jq_off::handles);
        if (r.stwcx(b, (w & 0xFFFF) | ((w & 0xFFFF0000u) - 0x10000u))) break;
    }
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------ semaphores

s32 cellSpursJobQueueSemaphoreInitialize(PPUContext& ctx, u32 sem, u32 jq)
{
    if (!sem || !jq) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if ((sem & 0x7F) || (jq & 0x7F)) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!mapped(ctx, sem, 16, "cellSpursJobQueueSemaphoreInitialize", "semaphore")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    st64(b, sem + 8, 0);
    st64(b, sem + 0, 0);
    st32(b, sem + sem_off::queue, jq);
    return lv2::CELL_OK;
}

// The count, a signed 29-bit value in bits 60..32.
s64 sem_count(u64 v) { return static_cast<s64>(v << 3) >> 35; }
u64 with_count(u64 v, s64 count) { return (v & ~(0x1FFFFFFFull << 32)) | (static_cast<u64>(count) & 0x1FFFFFFF) << 32; }

// Takes `count` from the semaphore; with too few, a blocking caller records itself as its waiter (the count goes
// negative by what it lacks) and sleeps until the policy module's releases bring it back to 0 or more.
s32 semaphore_acquire(PPUContext& ctx, u32 sem, u32 count, bool blocking)
{
    if (!sem) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (sem & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (count > 0x0FFFFFFF) return err(CELL_SPURS_JOB_ERROR_INVAL);
    const char* fn = blocking ? "cellSpursJobQueueSemaphoreAcquire" : "cellSpursJobQueueSemaphoreTryAcquire";
    if (!mapped(ctx, sem, 16, fn, "semaphore")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 jq = ps_read32(b, sem + sem_off::queue);
    const u32 fiber = fiber_self(ctx);
    u32 bit = 0; // 0: none taken (libspurs_jq's test; a taken bit 0 is taken again and bit 0 kept, as libspurs_jq does)
    for (;;) {
        Reserve r;
        const u64 v = r.ldarx(b, sem);
        if (static_cast<s64>(v) < 0) return err(CELL_SPURS_JOB_ERROR_BUSY);
        const s64 left = sem_count(v) - static_cast<s64>(count);
        const bool lacks = static_cast<s32>(left) < 0;
        u64 n = v;
        bool need_bit = false;
        if (lacks) {
            if (!blocking) return err(CELL_SPURS_JOB_ERROR_AGAIN);
            if (fiber) {
                n = (n | 1ull << 63) & ~(3ull << 61);
                n = (n & ~0xFFFFFFFFull) | fiber;
            } else if (!bit) {
                need_bit = true;
            } else {
                n = (n | 1ull << 63) & ~(3ull << 61);
                n |= 2ull << 61;
                n = (n & ~0xFFFFFFFFull) | bit;
            }
        }
        if (!need_bit) n = with_count(n, left);
        if (!r.stdcx(b, n)) continue;
        if (need_bit) {
            if (!queue_mapped(ctx, jq, fn)) return err(CELL_SPURS_JOB_ERROR_INVAL);
            bit = reserve_bit(b, jq);
            continue;
        }
        if (lacks) {
            if (fiber) {
                if (fiber_wait_signal(ctx) != lv2::CELL_OK)
                    jq_assert("job_queue_semaphore.cpp", 172, "_cellSpursJobQueueSemaphoreAcquire");
                if (static_cast<s32>(static_cast<u32>(ps_read64(b, sem) >> 29)) < 0)
                    jq_assert("job_queue_semaphore.cpp", 173, "_cellSpursJobQueueSemaphoreAcquire");
            } else {
                const u32 id = ps_read32(b, jq + jq_off::event_flag0 + 4 * (bit / 64));
                if (sc(ctx, 85, {id, 1ull << (bit % 64), 0x11, 0, 0}) != lv2::CELL_OK)
                    jq_assert("job_queue_semaphore.cpp", 164, "_cellSpursJobQueueSemaphoreAcquire");
            }
        }
        if (bit) release_bit(b, jq, bit);
        lwsync();
        return lv2::CELL_OK;
    }
}

s32 cellSpursJobQueueSemaphoreTryAcquire(PPUContext& ctx, u32 sem, u32 count) { return semaphore_acquire(ctx, sem, count, false); }
s32 cellSpursJobQueueSemaphoreAcquire(PPUContext& ctx, u32 sem, u32 count) { return semaphore_acquire(ctx, sem, count, true); }

// cellSpursJobQueueSendSignal(job): a job that suspended to wait for a signal (bit 0x400000 of the word at 0x2C, set by
// the policy module, with the queue at 0x28) goes on the queue's list of signalled jobs and the workload is kicked;
// else the signal is left in the job (0x800000) for its wait to find.
s32 cellSpursJobQueueSendSignal(PPUContext& ctx, u32 job)
{
    if (!job) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (job & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    const char* fn = "cellSpursJobQueueSendSignal";
    if (!mapped(ctx, job, 0x30, fn, "job")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    bool waiting = false;
    u32 index = 0;
    for (Reserve r;;) {
        const u32 w = r.lwarx(b, job + 0x2C);
        waiting = (w >> 22) & 1;
        u32 n = w & ~0x00400000u;
        if (waiting) {
            index = (w >> 8) & 0xFF;
            n &= ~0x0080FF00u; // a signal left earlier is taken with it
        } else {
            n |= 0x00800000u;
        }
        if (r.stwcx(b, n)) break;
    }
    if (!waiting) return lv2::CELL_OK;
    const u32 jq = ps_read32(b, job + 0x28);
    if (!queue_mapped(ctx, jq, fn)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    for (Reserve r;;) {
        const u32 head = r.lwarx(b, jq + jq_off::signal_list);
        st32(b, job, head);
        lwsync();
        if (r.stwcx(b, job + index)) break;
    }
    lwsync();
    const u64 control = ps_read64(b, jq + jq_off::control);
    if (!(control & 0x4000) && !(control >> 56)) kick(ctx, b, jq);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------------ ports

u32 cellSpursJobQueuePortGetJobQueue(PPUContext& ctx, u32 port)
{
    if (!mapped(ctx, port + port_off::queue, 4, "cellSpursJobQueuePortGetJobQueue", "port")) return 0;
    return ps_read32(gb(ctx), port + port_off::queue);
}

bool port_mapped(PPUContext& ctx, u32 port, std::string_view fn) { return mapped(ctx, port, port_off::size, fn, "port"); }

s32 _cellSpursJobQueuePortPushSync(PPUContext& ctx, u32 port, u32 mask, u8 blocking)
{
    if (!port) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (port & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!port_mapped(ctx, port, "_cellSpursJobQueuePortPushSync")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    return _cellSpursJobQueuePushSync(ctx, ps_read32(b, port + port_off::queue),
        static_cast<s32>(ps_read32(b, port + port_off::handle)), mask, blocking);
}

s32 _cellSpursJobQueuePortPushFlush(PPUContext& ctx, u32 port, u8 blocking)
{
    if (!port) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (port & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!port_mapped(ctx, port, "_cellSpursJobQueuePortPushFlush")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    return _cellSpursJobQueuePushFlush(ctx, ps_read32(b, port + port_off::queue),
        static_cast<s32>(ps_read32(b, port + port_off::handle)), blocking);
}

// The port's count of the jobs its sync waits for (at 0x10, at most 0x0FFFFFFF), raised by `n` before a push: through
// the reservation system for a multi-threaded port (busy while a sync runs), plainly for one thread's. False: BUSY.
// `limit` is libspurs_jq's bound on the count before the addition (0x0FFFFFFE for one job), or on the sum (job lists).
bool port_count_add(u8* b, u32 port, u32 n, bool sum_limit)
{
    if (ps_read8(b, port + port_off::multi)) {
        for (Reserve r;;) {
            const u64 v = r.ldarx(b, port + port_off::counter);
            if ((v >> 8) & 0xFF) return false;
            const u32 count = static_cast<u32>(v >> 32);
            const u32 sum = count + n;
            if (sum_limit ? sum > 0x0FFFFFFF : count > 0x0FFFFFFE) return false;
            if (r.stdcx(b, (v & 0xFFFFFFFFull) | u64{sum} << 32)) return true;
        }
    }
    const u32 count = ps_read32(b, port + port_off::count);
    const u32 sum = count + n;
    if (sum_limit ? sum > 0x0FFFFFFF : count > 0x0FFFFFFE) return false;
    st32(b, port + port_off::count, sum);
    return true;
}

// Takes `n` back after a push failed; a multi-threaded port being synced makes it BUSY (the push's error is lost).
s32 port_count_undo(u8* b, u32 port, u32 n, s32 r)
{
    if (!ps_read8(b, port + port_off::multi)) {
        st32(b, port + port_off::count, ps_read32(b, port + port_off::count) - n);
        return r;
    }
    for (Reserve res;;) {
        const u64 v = res.ldarx(b, port + port_off::counter);
        if ((v >> 8) & 0xFF) return err(CELL_SPURS_JOB_ERROR_BUSY);
        if (res.stdcx(b, (v & 0xFFFFFFFFull) | u64{static_cast<u32>(v >> 32) - n} << 32)) return r;
    }
}

// _cellSpursJobQueuePortPushJobListBody(port, jobList, tag, useSemaphore, isBlocking): the port's semaphore counts the
// list's jobs.
s32 _cellSpursJobQueuePortPushJobListBody(PPUContext& ctx, u32 port, u32 job_list, u32 tag, u8 use_sem, u8 blocking)
{
    if (!job_list || !port) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if ((job_list & 0xF) || (port & 0x7F)) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    const char* fn = "_cellSpursJobQueuePortPushJobListBody";
    if (!port_mapped(ctx, port, fn) || !mapped(ctx, job_list, 8, fn, "job list")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 size = ps_read32(b, job_list + 4);
    if (bad_size(size) || tag > 15) return err(CELL_SPURS_JOB_ERROR_INVAL);
    const u32 jq = ps_read32(b, port + port_off::queue);
    if (!jq || (jq & 0x7F)) return err(CELL_SPURS_JOB_ERROR_STAT);
    if (!queue_mapped(ctx, jq, fn)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    const u32 jobs = ps_read32(b, job_list);
    u64 sem = 0;
    if (use_sem) {
        if (!port_count_add(b, port, jobs, true)) return err(CELL_SPURS_JOB_ERROR_BUSY);
        sem = u64{port} << 32;
    }
    lwsync();
    const s32 r = push_command(ctx, jq, ps_read32(b, port + port_off::handle),
        u64{tag} << 35 | job_list | sem | kCmdJobList, blocking != 0);
    if (r == lv2::CELL_OK || !use_sem) return r;
    return port_count_undo(b, port, ps_read32(b, job_list), r);
}

// libspurs_jq's port push of one job (its descriptor as the game has it).
s32 port_push_job(PPUContext& ctx, std::string_view fn, u32 port, u32 job, u32 size, u32 tag, u8 use_sem, u8 flag,
    u8 blocking)
{
    if (!job || !port) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if ((job & 0xF) || (port & 0x7F)) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!port_mapped(ctx, port, fn)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 jq = ps_read32(b, port + port_off::queue);
    if (!jq || (jq & 0x7F)) return err(CELL_SPURS_JOB_ERROR_STAT);
    if (!queue_mapped(ctx, jq, fn)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (bad_size(size) || ps_read32(b, jq + jq_off::max_size) < size || tag > 15) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u64 sem = 0;
    if (use_sem) {
        if (!port_count_add(b, port, 1, false)) return err(CELL_SPURS_JOB_ERROR_BUSY);
        sem = u64{port} << 32;
    }
    lwsync();
    const s32 r = push_command(ctx, jq, ps_read32(b, port + port_off::handle),
        job | sem | u64{flag != 0} << 34 | u64{tag} << 35 | size_bits(size), blocking != 0);
    if (r == lv2::CELL_OK || !use_sem) return r;
    return port_count_undo(b, port, 1, r);
}

s32 _cellSpursJobQueuePortPushJobBody(PPUContext& ctx, u32 port, u32 job, u32 size, u32 tag, u8 use_sem, u8 blocking)
{
    return port_push_job(ctx, "_cellSpursJobQueuePortPushJobBody", port, job, size, tag, use_sem, 0, blocking);
}
s32 _cellSpursJobQueuePortPushJobBody2(PPUContext& ctx, u32 port, u32 job, u32 size, u32 tag, u8 use_sem, u8 flag,
    u8 blocking)
{
    return port_push_job(ctx, "_cellSpursJobQueuePortPushJobBody2", port, job, size, tag, use_sem, flag, blocking);
}
s32 _cellSpursJobQueuePortPushBody(PPUContext& ctx, u32 port, u32 job, u32 size, u8 use_sem, u8 blocking)
{
    return port_push_job(ctx, "_cellSpursJobQueuePortPushBody", port, job, size, 0, use_sem, 0, blocking);
}

// A port's sync: takes the jobs it counted from its semaphore (blocking or not) and, done, starts the count and the copy
// buffer afresh. Busy while it runs (byte 0x16) on a multi-threaded port. libspurs_jq's slips, kept: before SDK 3.3 a
// multi-threaded port's busy mark is stored with stwcx. at the doubleword's address (the count word gets the low word);
// a single-threaded port's final store leaves in byte 0x17 the low byte of the caller's r31.
s32 port_sync(PPUContext& ctx, u32 port, bool blocking)
{
    if (!port) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (port & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    const char* fn = blocking ? "cellSpursJobQueuePortSync" : "cellSpursJobQueuePortTrySync";
    if (!port_mapped(ctx, port, fn)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 jq = ps_read32(b, port + port_off::queue);
    if (!queue_mapped(ctx, jq, fn)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    const u32 sdk = ps_read32(b, jq + jq_off::sdk_version);
    u64 v = ctx.gpr[31];
    u32 count = 0, slot = 0;
    if (ps_read8(b, port + port_off::multi)) {
        for (Reserve r;;) {
            const u64 old = r.ldarx(b, port + port_off::counter);
            if ((old >> 8) & 0xFF) return err(CELL_SPURS_JOB_ERROR_BUSY);
            v = (old & ~0xFF00ull) | 0x100;
            count = static_cast<u32>(v >> 32);
            slot = static_cast<u32>(v >> 16);
            const bool ok = sdk > 0x32FFFF ? r.stdcx(b, v) : r.stwcx_at(b, port + port_off::counter, static_cast<u32>(v));
            if (ok) break;
        }
    } else {
        count = ps_read32(b, port + port_off::count);
        if (count > 0x0FFFFFFE) return err(CELL_SPURS_JOB_ERROR_BUSY);
        slot = ps_read16(b, port + port_off::slot);
    }
    const s32 r = semaphore_acquire(ctx, port, count, blocking);
    if (r == lv2::CELL_OK) count = slot = 0;
    v = (v & 0xFFull) | u64{count} << 32 | u64{slot & 0xFFFF} << 16;
    st64(b, port + port_off::counter, v);
    return r;
}

s32 cellSpursJobQueuePortTrySync(PPUContext& ctx, u32 port) { return port_sync(ctx, port, false); }
s32 cellSpursJobQueuePortSync(PPUContext& ctx, u32 port) { return port_sync(ctx, port, true); }

// Where libspurs_jq's own frame would hold the handle cellSpursJobQueueOpen returns (r1 - frame + 0x70): a port keeps
// what is there when Open fails, as libspurs_jq's does.
s32 open_into_frame(PPUContext& ctx, u32 jq, u32 frame, u32& handle)
{
    const u32 slot = static_cast<u32>(ctx.gpr[1]) - frame + 0x70;
    const s32 r = cellSpursJobQueueOpen(ctx, jq, slot);
    handle = ps_read32(gb(ctx), slot);
    return r;
}

s32 cellSpursJobQueuePortInitialize(PPUContext& ctx, u32 port, u32 jq, u8 multi)
{
    if (!port || !jq) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if ((port & 0x7F) || (jq & 0x7F)) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!port_mapped(ctx, port, "cellSpursJobQueuePortInitialize")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    zero(b, port, port_off::size);
    ps_write8(b, port + port_off::multi, multi);
    if (cellSpursJobQueueSemaphoreInitialize(ctx, port, jq) != lv2::CELL_OK)
        jq_assert("job_queue_port.cpp", 64, "cellSpursJobQueuePortInitialize");
    ps_write8(b, port + port_off::initialized, 1);
    ps_write32(b, port + port_off::queue, jq);
    u32 handle = 0;
    const s32 r = open_into_frame(ctx, jq, 0xB0, handle);
    ps_write32(b, port + port_off::handle, handle);
    return r;
}

s32 cellSpursJobQueuePortInitializeWithDescriptorBuffer(PPUContext& ctx, u32 port, u32 jq, u32 buffer, u32 size, u32 slots,
    u8 multi)
{
    if (!buffer) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (buffer & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (bad_size(size)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    const s32 r = cellSpursJobQueuePortInitialize(ctx, port, jq, multi);
    if (r != lv2::CELL_OK) return r;
    u8* b = gb(ctx);
    ps_write16(b, port + port_off::slot_size, static_cast<u16>(size));
    ps_write32(b, port + port_off::buffer, buffer);
    ps_write16(b, port + port_off::slots, static_cast<u16>(slots));
    return lv2::CELL_OK;
}

s32 cellSpursJobQueuePortFinalize(PPUContext& ctx, u32 port)
{
    if (!port) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (port & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!port_mapped(ctx, port, "cellSpursJobQueuePortFinalize")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    if (!ps_read8(b, port + port_off::initialized)) return err(CELL_SPURS_JOB_ERROR_STAT);
    if (cellSpursJobQueueClose(ctx, ps_read32(b, port + port_off::queue),
            static_cast<s32>(ps_read32(b, port + port_off::handle))) != lv2::CELL_OK)
        jq_assert("job_queue_port.cpp", 94, "cellSpursJobQueuePortFinalize");
    ps_write8(b, port + port_off::initialized, 0);
    return lv2::CELL_OK;
}

// A port's copy push: the descriptor copied into the port's next buffer slot (16 bytes at a time, the size rounded down
// to 16 as libspurs_jq's vector copy does), the copy pushed. The slots are reused after a sync.
s32 port_copy_push(PPUContext& ctx, std::string_view fn, u32 port, u32 job, u32 size, u32 tag, u8 use_sem, u8 flag,
    u8 blocking)
{
    if (!job || !port) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if ((job & 0xF) || (port & 0x7F)) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!port_mapped(ctx, port, fn)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 jq = ps_read32(b, port + port_off::queue);
    if (!jq || (jq & 0x7F)) return err(CELL_SPURS_JOB_ERROR_STAT);
    if (!queue_mapped(ctx, jq, fn)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (bad_size(size) || ps_read32(b, jq + jq_off::max_size) < size || tag > 15) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!ps_read8(b, port + port_off::initialized)) return err(CELL_SPURS_JOB_ERROR_STAT);
    const u32 buffer = ps_read32(b, port + port_off::buffer);
    if (!buffer) return err(CELL_SPURS_JOB_ERROR_PERM);
    if (ps_read16(b, port + port_off::slot_size) < size) return err(CELL_SPURS_JOB_ERROR_INVAL);
    const u32 slots = ps_read16(b, port + port_off::slots);
    const u32 add = use_sem;
    u32 slot = 0;
    if (ps_read8(b, port + port_off::multi)) {
        for (Reserve r;;) {
            const u64 v = r.ldarx(b, port + port_off::counter);
            slot = static_cast<u32>(v >> 16) & 0xFFFF;
            if (slot == slots) return err(CELL_SPURS_JOB_ERROR_AGAIN);
            if ((v >> 8) & 0xFF) return err(CELL_SPURS_JOB_ERROR_BUSY);
            const u32 count = static_cast<u32>(v >> 32);
            if (count > 0x0FFFFFFE) return err(CELL_SPURS_JOB_ERROR_BUSY);
            const u64 n = (v & 0xFFFFull) | u64{(slot + 1) & 0xFFFF} << 16 | u64{count + add} << 32;
            if (r.stdcx(b, n)) break;
        }
    } else {
        slot = ps_read16(b, port + port_off::slot);
        if (slot == slots) return err(CELL_SPURS_JOB_ERROR_AGAIN);
        const u32 count = ps_read32(b, port + port_off::count);
        if (count > 0x0FFFFFFE) return err(CELL_SPURS_JOB_ERROR_BUSY);
        st16(b, port + port_off::slot, static_cast<u16>(slot + 1));
        st32(b, port + port_off::count, count + add);
    }
    const u32 dest = buffer + ps_read16(b, port + port_off::slot_size) * slot;
    const u32 bytes = size & ~15u;
    if (!mapped(ctx, job, bytes, fn, "job") || !mapped(ctx, dest, bytes, fn, "descriptor buffer"))
        return err(CELL_SPURS_JOB_ERROR_INVAL);
    std::memmove(b + dest, b + job, bytes);
    stored(dest, bytes);
    const u64 sem = use_sem ? u64{port} << 32 : 0;
    lwsync();
    const s32 r = push_command(ctx, jq, ps_read32(b, port + port_off::handle),
        dest | sem | u64{flag != 0} << 34 | u64{tag} << 35 | size_bits(size), blocking != 0);
    if (r == lv2::CELL_OK || !use_sem) return r;
    return port_count_undo(b, port, 1, r);
}

s32 _cellSpursJobQueuePortCopyPushJobBody(PPUContext& ctx, u32 port, u32 job, u32 size, u32 tag, u8 use_sem, u8 blocking)
{
    return port_copy_push(ctx, "_cellSpursJobQueuePortCopyPushJobBody", port, job, size, tag, use_sem, 0, blocking);
}
s32 _cellSpursJobQueuePortCopyPushJobBody2(PPUContext& ctx, u32 port, u32 job, u32 size, u32 tag, u8 use_sem, u8 flag,
    u8 blocking)
{
    return port_copy_push(ctx, "_cellSpursJobQueuePortCopyPushJobBody2", port, job, size, tag, use_sem, flag, blocking);
}
s32 _cellSpursJobQueuePortCopyPushBody(PPUContext& ctx, u32 port, u32 job, u32 size, u8 use_sem, u8 blocking)
{
    return port_copy_push(ctx, "_cellSpursJobQueuePortCopyPushBody", port, job, size, 0, use_sem, 0, blocking);
}

// ----------------------------------------------------------------------------------------------------- port2s

u32 cellSpursJobQueuePort2GetJobQueue(PPUContext& ctx, u32 port)
{
    if (!mapped(ctx, port + port2_off::queue, 4, "cellSpursJobQueuePort2GetJobQueue", "port")) return 0;
    return ps_read32(gb(ctx), port + port2_off::queue);
}

bool port2_mapped(PPUContext& ctx, u32 port, std::string_view fn) { return mapped(ctx, port, port2_off::size, fn, "port"); }

// Port2 flags: bit 2 = do not wait (several take only it: libspurs_jq tests flags & ~4).
s32 cellSpursJobQueuePort2PushSync(PPUContext& ctx, u32 port, u32 mask, u32 flags)
{
    if (flags & ~4u) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!port) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (port & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!port2_mapped(ctx, port, "cellSpursJobQueuePort2PushSync")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    return _cellSpursJobQueuePushSync(ctx, ps_read32(b, port + port2_off::queue),
        static_cast<s32>(ps_read32(b, port + port2_off::handle)), mask, !(flags & 4));
}

s32 cellSpursJobQueuePort2PushFlush(PPUContext& ctx, u32 port, u32 flags)
{
    if (flags & ~4u) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!port) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (port & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!port2_mapped(ctx, port, "cellSpursJobQueuePort2PushFlush")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    return _cellSpursJobQueuePushFlush(ctx, ps_read32(b, port + port2_off::queue),
        static_cast<s32>(ps_read32(b, port + port2_off::handle)), !(flags & 4));
}

// A port2's count (busy: byte 0x14), always through the reservation system.
bool port2_count_add(u8* b, u32 port, u32 n, bool sum_limit)
{
    for (Reserve r;;) {
        const u64 v = r.ldarx(b, port + port2_off::counter);
        if ((v >> 24) & 0xFF) return false;
        const u32 count = static_cast<u32>(v >> 32);
        const u32 sum = count + n;
        if (sum_limit ? sum > 0x0FFFFFFF : count > 0x0FFFFFFE) return false;
        if (r.stdcx(b, (v & 0xFFFFFFFFull) | u64{sum} << 32)) return true;
    }
}

s32 port2_count_undo(u8* b, u32 port, u32 n, s32 r)
{
    for (Reserve res;;) {
        const u64 v = res.ldarx(b, port + port2_off::counter);
        if ((v >> 24) & 0xFF) return err(CELL_SPURS_JOB_ERROR_BUSY);
        if (res.stdcx(b, (v & 0xFFFFFFFFull) | u64{static_cast<u32>(v >> 32) - n} << 32)) return r;
    }
}

// _cellSpursJobQueuePort2PushJobListBody(port, jobList, tag, flags): flags bit 0 = count the list's jobs in the port's
// semaphore, bit 2 = do not wait.
s32 _cellSpursJobQueuePort2PushJobListBody(PPUContext& ctx, u32 port, u32 job_list, u32 tag, u32 flags)
{
    if (flags & ~5u) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!job_list || !port) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if ((job_list & 0xF) || (port & 0x7F)) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    const char* fn = "_cellSpursJobQueuePort2PushJobListBody";
    if (!port2_mapped(ctx, port, fn) || !mapped(ctx, job_list, 8, fn, "job list")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 size = ps_read32(b, job_list + 4);
    if (bad_size(size) || tag > 15) return err(CELL_SPURS_JOB_ERROR_INVAL);
    const u32 jq = ps_read32(b, port + port2_off::queue);
    if (!jq || (jq & 0x7F)) return err(CELL_SPURS_JOB_ERROR_STAT);
    if (!queue_mapped(ctx, jq, fn)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    const bool use_sem = flags & 1;
    u64 sem = 0;
    if (use_sem) {
        if (!port2_count_add(b, port, ps_read32(b, job_list), true)) return err(CELL_SPURS_JOB_ERROR_BUSY);
        sem = u64{port} << 32;
    }
    lwsync();
    const s32 r = push_command(ctx, jq, ps_read32(b, port + port2_off::handle),
        kCmdJobList | job_list | u64{tag} << 35 | sem, !(flags & 4));
    if (r == lv2::CELL_OK || !use_sem) return r;
    return port2_count_undo(b, port, ps_read32(b, job_list), r);
}

s32 cellSpursJobQueuePort2Sync(PPUContext& ctx, u32 port, u32 flags)
{
    if (flags & ~4u) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!port) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (port & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!port2_mapped(ctx, port, "cellSpursJobQueuePort2Sync")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    u64 v = 0;
    for (Reserve r;;) {
        const u64 old = r.ldarx(b, port + port2_off::counter);
        if ((old >> 24) & 0xFF) return err(CELL_SPURS_JOB_ERROR_BUSY);
        v = (old & ~(0xFFull << 24)) | 1ull << 24;
        if (r.stdcx(b, v)) break;
    }
    u32 count = static_cast<u32>(v >> 32);
    const s32 r = semaphore_acquire(ctx, port, count, !(flags & 4));
    if (r == lv2::CELL_OK) count = 0;
    v = (v & 0x00FFFFFFull) | u64{count} << 32;
    st64(b, port + port2_off::counter, v);
    return r;
}

s32 cellSpursJobQueuePort2Create(PPUContext& ctx, u32 port, u32 jq)
{
    if (!port || !jq) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if ((port & 0x7F) || (jq & 0x7F)) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!port2_mapped(ctx, port, "cellSpursJobQueuePort2Create")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    zero(b, port, port2_off::size);
    if (cellSpursJobQueueSemaphoreInitialize(ctx, port, jq) != lv2::CELL_OK)
        jq_assert("job_queue_port2.cpp", 58, "cellSpursJobQueuePort2Create");
    ps_write8(b, port + port2_off::created, 1);
    ps_write32(b, port + port2_off::queue, jq);
    u32 handle = 0;
    const s32 r = open_into_frame(ctx, jq, 0xA0, handle);
    ps_write32(b, port + port2_off::handle, handle);
    return r;
}

s32 cellSpursJobQueuePort2Destroy(PPUContext& ctx, u32 port)
{
    if (!port) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (port & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!port2_mapped(ctx, port, "cellSpursJobQueuePort2Destroy")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    if (!ps_read8(b, port + port2_off::created) || ps_read8(b, port + port2_off::byte21) ||
        !ps_read32(b, port + port2_off::queue))
        return err(CELL_SPURS_JOB_ERROR_STAT);
    if (ps_read32(b, port + port2_off::count)) return err(CELL_SPURS_JOB_ERROR_BUSY);
    if (cellSpursJobQueueClose(ctx, ps_read32(b, port + port2_off::queue),
            static_cast<s32>(ps_read32(b, port + port2_off::handle))) != lv2::CELL_OK)
        jq_assert("job_queue_port2.cpp", 93, "cellSpursJobQueuePort2Destroy");
    ps_write8(b, port + port2_off::created, 0);
    return lv2::CELL_OK;
}

s32 cellSpursJobQueuePort2AllocateJobDescriptor(PPUContext& ctx, u32 port, u32 size, u32 flags, u32 out)
{
    if (flags & ~4u) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!port) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (port & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    const char* fn = "cellSpursJobQueuePort2AllocateJobDescriptor";
    if (!port2_mapped(ctx, port, fn)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 jq = ps_read32(b, port + port2_off::queue);
    if (!queue_mapped(ctx, jq, fn)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    return allocate_descriptor(ctx, jq, ps_read32(b, port + port2_off::handle), size, !(flags & 4), out);
}

// A port2's push of one job: 0 the descriptor as it is (not one of the pool's), 1 a copy into a descriptor of the pool
// (`room` bytes, waiting for one) given back after the job, 2 one of the pool's given back after the job.
s32 port2_push_job(PPUContext& ctx, std::string_view fn, u32 port, u32 job, u32 size, u32 room, u32 tag, u32 flags, u32 mode)
{
    if (!job) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (job & 0xF) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (size > room) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (!port2_mapped(ctx, port, fn)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 jq = ps_read32(b, port + port2_off::queue);
    if (!jq || (jq & 0x7F)) return err(CELL_SPURS_JOB_ERROR_STAT);
    if (!queue_mapped(ctx, jq, fn)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if ((size & 0x7F) && size != 64) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (ps_read32(b, jq + jq_off::max_size) < size || tag > 15) return err(CELL_SPURS_JOB_ERROR_INVAL);
    const bool use_sem = flags & 1;
    if (use_sem && !port2_count_add(b, port, 1, false)) return err(CELL_SPURS_JOB_ERROR_BUSY);
    const u64 sem = use_sem ? u64{port} << 32 : 0;
    const u32 handle = ps_read32(b, port + port2_off::handle);
    const u64 bits = u64{(flags >> 1) & 1} << 34 | u64{tag} << 35 | size_bits(size) | sem;
    s32 r = lv2::CELL_OK;
    switch (mode) {
    case 0:
        if (in_pool(b, jq, job)) r = err(CELL_SPURS_JOB_ERROR_INVAL);
        else {
            lwsync();
            r = push_command(ctx, jq, handle, bits | job, !(flags & 4));
        }
        break;
    case 1: {
        StackVar out(ctx, b, 4);
        r = allocate_descriptor(ctx, jq, handle, room, true, out.addr());
        if (r != lv2::CELL_OK) break;
        const u32 copy = ps_read32(b, out.addr());
        const u32 bytes = size & ~15u;
        if (!mapped(ctx, job, bytes, fn, "job") || !mapped(ctx, copy, bytes, fn, "descriptor")) {
            r = err(CELL_SPURS_JOB_ERROR_INVAL);
            break;
        }
        std::memmove(b + copy, b + job, bytes);
        stored(copy, bytes);
        if (!in_pool(b, jq, copy)) r = err(CELL_SPURS_JOB_ERROR_INVAL);
        else {
            lwsync();
            r = push_command(ctx, jq, ps_read32(b, port + port2_off::handle), bits | copy | kCmdRelease, !(flags & 4));
        }
        break;
    }
    case 2:
        if (!in_pool(b, jq, job)) r = err(CELL_SPURS_JOB_ERROR_INVAL);
        else {
            lwsync();
            r = push_command(ctx, jq, handle, bits | job | kCmdRelease, !(flags & 4));
        }
        break;
    default:
        jq_assert("job_queue_port2.cpp", 176, "_cellSpursJobQueuePort2PushBodyInternal");
    }
    if (r == lv2::CELL_OK || !use_sem) return r;
    return port2_count_undo(b, port, 1, r);
}

s32 _cellSpursJobQueuePort2PushAndReleaseJobBody(PPUContext& ctx, u32 port, u32 job, u32 size, u32 tag, u32 flags)
{
    if (flags & ~7u) return err(CELL_SPURS_JOB_ERROR_INVAL);
    return port2_push_job(ctx, "_cellSpursJobQueuePort2PushAndReleaseJobBody", port, job, size, size, tag, flags, 2);
}
s32 _cellSpursJobQueuePort2CopyPushJobBody(PPUContext& ctx, u32 port, u32 job, u32 size, u32 room, u32 tag, u32 flags)
{
    if (flags & ~3u) return err(CELL_SPURS_JOB_ERROR_INVAL);
    return port2_push_job(ctx, "_cellSpursJobQueuePort2CopyPushJobBody", port, job, size, room, tag, flags, 1);
}
s32 _cellSpursJobQueuePort2PushJobBody(PPUContext& ctx, u32 port, u32 job, u32 size, u32 tag, u32 flags)
{
    if (flags & ~7u) return err(CELL_SPURS_JOB_ERROR_INVAL);
    return port2_push_job(ctx, "_cellSpursJobQueuePort2PushJobBody", port, job, size, size, tag, flags, 0);
}

// ------------------------------------------------------------------------------------------ exception handlers

// cellSpursJobQueueSetExceptionEventHandler(jobQueue, handler, arg) (also exported as ...Handler2: the same code).
s32 cellSpursJobQueueSetExceptionEventHandler(PPUContext& ctx, u32 jq, u32 handler, u32 arg)
{
    if (!jq || !handler) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (jq & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!queue_mapped(ctx, jq, "cellSpursJobQueueSetExceptionEventHandler")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    if ((ps_read32(b, jq + jq_off::spurs_wid) & 0x7F) > 31) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (ps_read64(b, jq + jq_off::handler)) return err(CELL_SPURS_JOB_ERROR_BUSY);
    st64(b, jq + jq_off::handler_arg, arg);
    st64(b, jq + jq_off::handler, handler);
    lwsync();
    return lv2::CELL_OK;
}

s32 cellSpursJobQueueSetExceptionEventHandler2(PPUContext& ctx, u32 jq, u32 handler, u32 arg)
{
    return cellSpursJobQueueSetExceptionEventHandler(ctx, jq, handler, arg);
}

s32 cellSpursJobQueueUnsetExceptionEventHandler(PPUContext& ctx, u32 jq)
{
    if (!jq) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (jq & 0x7F) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (!queue_mapped(ctx, jq, "cellSpursJobQueueUnsetExceptionEventHandler")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    if ((ps_read32(b, jq + jq_off::spurs_wid) & 0x7F) > 31) return err(CELL_SPURS_JOB_ERROR_INVAL);
    st64(b, jq + jq_off::handler_arg, 0);
    st64(b, jq + jq_off::handler, 0);
    lwsync();
    return lv2::CELL_OK;
}

// The workload's exception event handler (cellSpursSetExceptionEventHandler's, with the queue as its argument):
// libspurs_jq prints the queue's state and the SPU's (read with sys_spu_thread_read_ls from the policy module's local
// store), then calls the game's handler (cellSpursJobQueueSetExceptionEventHandler's) with the SPURS instance, the queue,
// the exception information, the job's information (0x44 bytes) gathered from local store, the game's argument and two
// words. pspocket's SPURS has no SPU threads and stops a worker whose policy module faults with a fatal log, so it never
// calls this; were it called, the state is logged (in pspocket's words) and the game's handler called with the job's
// information zeroed, as libspurs_jq's is when it cannot read local store.
void _pspocket_cellSpursJqExceptionHandler(PPUContext& ctx, u32 spurs, u32 info, u32 jq)
{
    u8* b = gb(ctx);
    if (!mapped(ctx, jq, jq_off::size, "the job queue's exception event handler", "job queue")) return;
    const u64 header = ps_read64(b, jq + jq_off::header);
    PS_LOG_ERROR(kChan, "job queue {:#x} (SPURS {:#x}, workload {}, revision {:#x}, SDK {:#x}): SPU exception; command ring "
                        "read {} write {} of {} at {:#x}, error {:#x} cause {:#x}; the SPU's state is not readable "
                        "(a native SPURS has no SPU threads)",
        jq, ps_read32(b, jq + jq_off::spurs_wid) & ~0x7Fu, ps_read32(b, jq + jq_off::spurs_wid) & 0x7F,
        ps_read32(b, jq + jq_off::jq_revision), ps_read32(b, jq + jq_off::sdk_version), header >> 48,
        (header >> 32) & 0xFFFF, ps_read16(b, jq + jq_off::depth), ps_read32(b, jq + jq_off::command_list),
        ps_read32(b, jq + jq_off::exit_code), ps_read64(b, jq + jq_off::cause));
    const u64 handler = ps_read64(b, jq + jq_off::handler);
    if (!handler) return;
    const u32 arg = ps_read32(b, jq + jq_off::handler_arg + 4);
    StackVar job_info(ctx, b, 0x50);
    StackVar words(ctx, b, 0x10);
    call_guest(ctx, static_cast<u32>(handler), {spurs, jq, info, job_info.addr(), arg, words.addr(), words.addr() + 8});
}

// ------------------------------------------------------------------------------- GetSuspendedJobSize

// cellSpursJobQueueGetSuspendedJobSize(job, size, mode, u32* out): the room a job suspended by the policy module takes
// (its binary, buffers, cache, scratch and stack, as the job queue's job runtime lays them out), from its descriptor and
// the "JOBCRT Ver" header of its binary.
s32 cellSpursJobQueueGetSuspendedJobSize(PPUContext& ctx, u32 job, u32 size, s32 mode, u32 out)
{
    if (!out) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if ((out & 3) || (job & 0xF)) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    if (mode > 1) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (size - 0x100 > 0x2FF || (size & 0x7F)) return err(CELL_SPURS_JOB_ERROR_INVAL);
    const char* fn = "cellSpursJobQueueGetSuspendedJobSize";
    if (!mapped(ctx, job, size, fn, "job") || !mapped(ctx, out, 4, fn, "output")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    u8* b = gb(ctx);
    const u32 ea_low = static_cast<u32>(ps_read64(b, job));
    if (!(ea_low & ~1u)) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
    if (ea_low & 0xE) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    const u32 size_out = ps_read32(b, job + 0x18), size_in = ps_read32(b, job + 0x14);
    const u32 dma_size = ps_read16(b, job + 0x0A), cache_size = ps_read32(b, job + 0x24);
    if ((size_out & 0xF) || (size_in & 0xF) || (dma_size & 7) || (cache_size & 7)) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    const u32 size_binary = ps_read16(b, job + 0x08);
    if (!size_binary) return err(CELL_SPURS_JOB_ERROR_INVAL);
    if (cache_size + dma_size > size - 48 || cache_size > 39) return err(CELL_SPURS_JOB_ERROR_INVAL);
    const u8 type = ps_read8(b, job + 0x2C);
    const u32 guard = ((type >> 1) & 1) << 4; // the memory check's 16 bytes after each buffer
    const u32 n_dma = dma_size >> 3;
    if (n_dma) {
        u32 total = 0;
        for (u32 i = 0; i < n_dma; i++) {
            const u64 e = ps_read64(b, job + 0x30 + 8 * i);
            const u32 hi = static_cast<u32>(e >> 32), ea = static_cast<u32>(e);
            const u32 n = hi & 0x7FFF;
            if (n) {
                if (!ea) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
                if (hi & 0x80000000u) return err(CELL_SPURS_JOB_ERROR_INVAL);
                if (n > 0x4000) return err(CELL_SPURS_JOB_ERROR_INVAL);
                if (n <= 15 && n - 1 > 1 && n != 4 && n != 8) return err(CELL_SPURS_JOB_ERROR_INVAL);
                if (((n > 16 ? 16 : n) - 1) & ea) return err(CELL_SPURS_JOB_ERROR_ALIGN);
            }
            total += (n + 15) & ~15u;
        }
        if (total > size_in) return err(CELL_SPURS_JOB_ERROR_INVAL);
    }
    const u32 n_cache = cache_size >> 3;
    u32 cache = 0;
    for (u32 i = 0; i < n_cache; i++) {
        const u64 e = ps_read64(b, job + 0x30 + 8 * n_dma + 8 * i);
        const u32 n = static_cast<u32>(e >> 32), ea = static_cast<u32>(e);
        cache += (guard + n + 127) & ~127u;
        if (!n) continue;
        if (!ea) return err(CELL_SPURS_JOB_ERROR_NULL_POINTER);
        if (n <= 15) return err(CELL_SPURS_JOB_ERROR_INVAL);
        if ((n & 0xF) || (ea & 0xF)) return err(CELL_SPURS_JOB_ERROR_ALIGN);
    }
    const u32 hi_words = (type & 4) ? ps_read32(b, job) : 0;
    const u32 scratch = ps_read16(b, job + 0x1E), stack_units = ps_read16(b, job + 0x1C);
    const u32 stack = stack_units ? stack_units << 4 : 0x2000;
    const u32 rounded_scratch = ((scratch ? guard : 0) + (scratch << 4) + 127) & ~127u;
    const u32 rounded_in = ((size_in ? guard : 0) + size_in + 127) & ~127u;
    const u32 rounded_out = ((size_out ? guard : 0) + size_out + 127) & ~127u;
    const u32 rounded_binary = (((hi_words + size_binary) << 4) + 127) & ~127u;
    const u32 stack_part = (guard + stack + rounded_scratch + 127) & ~127u;
    if (stack_part + rounded_binary + rounded_in + rounded_out + cache > 0x37800 - size * 4)
        return err(CELL_SPURS_JOB_ERROR_MEMORY_SIZE);

    // The binary's job runtime header.
    const u32 binary = ea_low & ~3u;
    if (!mapped(ctx, binary, 0x50, fn, "job binary")) return err(CELL_SPURS_JOB_ERROR_INVAL);
    static constexpr char kCrt[10] = {'J', 'O', 'B', 'C', 'R', 'T', ' ', 'V', 'e', 'r'};
    static constexpr char kInfo[10] = {'%', 'S', 'P', 'U', 'R', 'S', ' ', 'J', 'O', 'B'};
    const auto same = [&](u32 at, const char (&text)[10]) {
        for (u32 i = 0; i < 10; i++) {
            const char c = static_cast<char>(ps_read8(b, at + i));
            if (c != text[i]) return false;
        }
        return true;
    };
    if (!same(binary + 0x30, kCrt)) return err(CELL_SPURS_JOB_ERROR_PERM);
    const auto version = [&] {
        return static_cast<s32>(static_cast<s8>(ps_read8(b, binary + 0x3A))) * 16 +
            static_cast<s8>(ps_read8(b, binary + 0x3B));
    };
    s32 v = version();
    bool info_ok = false;
    if (v == 0x342) {
        const u32 at = binary + ps_read32(b, binary + 0x3C) + 0x10;
        if (!mapped(ctx, at, 10, fn, "job binary")) return err(CELL_SPURS_JOB_ERROR_INVAL);
        info_ok = same(at, kInfo);
    }
    if (!info_ok && static_cast<u32>(v - 0x330) <= 18) return err(CELL_SPURS_JOB_ERROR_PERM);
    const u32 tag = ps_read32(b, binary + 0x20);
    const bool bin2 = tag == 0x42494E32u || tag == 0x62696E32u; // "BIN2", "bin2"
    u32 image = bin2 ? u32{ps_read16(b, binary + 0x2E)} << 4 : ps_read32(b, binary + 0x3C) - ps_read32(b, binary + 0x40);
    v = version();
    if (static_cast<u32>(v - 0x330) > 17) {
        u32 at = bin2 ? binary + 0x50 : ps_read32(b, binary + 0x3C) + 0x20 + binary;
        for (;;) {
            if (!mapped(ctx, at, 8, fn, "job binary")) return err(CELL_SPURS_JOB_ERROR_INVAL);
            if (ps_read32(b, at) == 0xFFFFFFFFu) break;
            image += ps_read32(b, at + 4);
            at += 8;
        }
    }
    const u32 guard2 = (type & 2) ? 16 : 0;
    u32 cache2 = 0;
    const u32 cache_entries = job + 0x30 + (ps_read16(b, job + 0x0A) & 0xFFF8);
    const s32 n_cache2 = static_cast<s32>(ps_read32(b, job + 0x24)) / 8;
    for (s32 i = 0; i < n_cache2; i++) cache2 += (guard2 + ps_read32(b, cache_entries + 8 * static_cast<u32>(i)) + 127) & ~127u;
    if (mode <= 0) image = (((ps_read16(b, job + 0x08) + ps_read32(b, job)) << 4) + 127 & ~127u) + cache2;
    const u32 out_bytes = ps_read32(b, job + 0x18), in_bytes = ps_read32(b, job + 0x14);
    const u32 scratch2 = ps_read16(b, job + 0x1E), stack2 = ps_read16(b, job + 0x1C);
    const u32 stack_bytes = stack2 ? ((stack2 << 4) + 127) & ~127u : 0x2000;
    const u32 in_part = ((in_bytes ? guard2 : 0) + in_bytes + 127) & ~127u;
    const u32 scratch_part = ((scratch2 ? guard2 : 0) + (scratch2 << 4) + 127) & ~127u;
    const u32 out_part = ((out_bytes ? guard2 : 0) + out_bytes + 127) & ~127u;
    ps_write32(b, out, stack_bytes + image + size + guard2 + out_part + in_part + scratch_part + 0x500);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------ registration

// libspurs_jq's imports from other libraries, linked to the firmware's modules when pspocket has no implementation.
constexpr HostImport kImports[] = {
    {"cellSpursJq", "cellFiber", kFiberSelf, false},       // cellFiberPpuSelf
    {"cellSpursJq", "cellFiber", kFiberWaitSignal, false}, // cellFiberPpuWaitSignal
};

} // namespace

void register_cellSpursJq(Registry& r)
{
    r.add_imports(kModule, kImports);
    r.add(kModule, {
        PS_HLE_FUNC(0x01BBF2E0, cellSpursJobQueueGetSpurs),
        PS_HLE_FUNC(0x0582338A, cellSpursJobQueueAttributeSetDoBusyWaiting),
        PS_HLE_FUNC(0x0D69929E, _cellSpursJobQueueAllocateJobDescriptorBody),
        PS_HLE_FUNC(0x0F03F712, cellSpursJobQueueAttributeSetSubmitWithEntryLock),
        PS_HLE_FUNC(0x13671514, cellSpursJobQueueSetWaitingMode),
        PS_HLE_FUNC(0x15934401, cellSpursJobQueueGetSuspendedJobSize),
        PS_HLE_FUNC(0x1686957E, cellSpursJobQueueAttributeSetMaxSizeJobDescriptor),
        PS_HLE_FUNC(0x1917359D, _cellSpursJobQueuePortCopyPushJobBody),
        PS_HLE_FUNC(0x1B6EEA7E, _cellSpursJobQueuePortPushJobListBody),
        PS_HLE_FUNC(0x1DA890C7, _cellSpursJobQueuePushAndReleaseJobBody),
        PS_HLE_FUNC(0x26FA81B4, cellSpursJobQueuePortFinalize),
        PS_HLE_FUNC(0x29C2ACC5, cellSpursJobQueueClose),
        PS_HLE_FUNC(0x2E775550, cellSpursJobQueueGetError),
        PS_HLE_FUNC(0x36D0A53C, _cellSpursJobQueuePushJob2Body),
        PS_HLE_FUNC(0x3D1294FC, cellSpursJobQueuePortInitialize),
        PS_HLE_FUNC(0x435BFF07, cellSpursJobQueuePort2GetJobQueue),
        PS_HLE_FUNC(0x43DDAB4F, cellSpursJobQueueAttributeInitialize),
        PS_HLE_FUNC(0x46C3FB5A, _cellSpursJobQueuePortCopyPushBody),
        PS_HLE_FUNC(0x4E1A17A6, _cellSpursJobQueuePortPushBody),
        PS_HLE_FUNC(0x5659DA82, cellSpursJobQueueGetMaxSizeJobDescriptor),
        PS_HLE_FUNC(0x5FB9B05D, cellSpursJobQueueAttributeSetMaxGrab),
        PS_HLE_FUNC(0x634B1502, cellSpursJobQueuePortSync),
        PS_HLE_FUNC(0x677027AF, cellSpursJobQueueSemaphoreInitialize),
        PS_HLE_FUNC(0x6C61BF87, cellSpursJobQueueSetExceptionEventHandler2),
        PS_HLE_FUNC(0x6C93EA18, cellSpursJobQueueSemaphoreAcquire),
        PS_HLE_FUNC(0x701FD8A9, _cellSpursJobQueuePushSync),
        PS_HLE_FUNC(0x742CEC0D, cellSpursJobQueueAttributeSetIsJobTypeMemoryCheck),
        PS_HLE_FUNC(0x80A0264C, cellSpursJobQueuePortTrySync),
        PS_HLE_FUNC(0x840C5239, cellSpursJobQueueGetHandleCount),
        PS_HLE_FUNC(0x85CD04CD, _cellSpursJobQueuePortPushJobBody),
        PS_HLE_FUNC(0x8C97A96C, cellSpursShutdownJobQueue),
        PS_HLE_FUNC(0x8CE4CFFA, cellSpursJoinJobQueue),
        PS_HLE_FUNC(0x90C88F84, cellSpursJobQueuePortGetJobQueue),
        PS_HLE_FUNC(0x90E392CF, _cellSpursJobQueuePortPushJobBody2),
        PS_HLE_FUNC(0x915455B3, cellSpursJobQueueSendSignal),
        PS_HLE_FUNC(0x93248B92, cellSpursJobQueueUnsetExceptionEventHandler),
        PS_HLE_FUNC(0x934ABB00, cellSpursJobQueuePort2AllocateJobDescriptor),
        PS_HLE_FUNC(0x9396BE1D, _cellSpursJobQueuePortPushSync),
        PS_HLE_FUNC(0x97409F67, cellSpursJobQueueSemaphoreTryAcquire),
        PS_HLE_FUNC(0x98203E3C, _cellSpursJobQueuePushJobBody2),
        PS_HLE_FUNC(0x983BE7FC, _cellSpursCreateJobQueueWithJobDescriptorPool),
        PS_HLE_FUNC(0x99316997, _cellSpursJobQueuePushBody),
        PS_HLE_FUNC(0x9C300489, cellSpursJobQueueSetExceptionEventHandler),
        PS_HLE_FUNC(0xAF1C7A1D, _cellSpursJobQueuePushJobListBody),
        PS_HLE_FUNC(0xB53436E7, _cellSpursJobQueuePort2PushJobListBody),
        PS_HLE_FUNC(0xBD1C5D6B, _cellSpursJobQueuePortPushFlush),
        PS_HLE_FUNC(0xBFEA28AB, cellSpursJobQueuePort2PushFlush),
        PS_HLE_FUNC(0xC343EE10, cellSpursGetJobQueueId),
        PS_HLE_FUNC(0xC39173A4, _cellSpursJobQueuePort2CopyPushJobBody),
        PS_HLE_FUNC(0xC4843B74, _cellSpursJobQueuePushJobBody),
        PS_HLE_FUNC(0xC485D207, cellSpursJobQueuePort2Destroy),
        PS_HLE_FUNC(0xCF89F218, _cellSpursJobQueuePort2PushJobBody),
        PS_HLE_FUNC(0xDF0120C2, _cellSpursJobQueuePushFlush),
        PS_HLE_FUNC(0xE1731DF3, _cellSpursJobQueuePort2PushAndReleaseJobBody),
        PS_HLE_FUNC(0xE70F874E, cellSpursJobQueueAttributeSetGrabParameters),
        PS_HLE_FUNC(0xEF66C4B7, _cellSpursJobQueuePortCopyPushJobBody2),
        PS_HLE_FUNC(0xF11FE0F1, cellSpursJobQueuePort2Create),
        PS_HLE_FUNC(0xF244E799, _cellSpursCreateJobQueue),
        PS_HLE_FUNC(0xF5DE6363, cellSpursJobQueuePortInitializeWithDescriptorBuffer),
        PS_HLE_FUNC(0xFACB3CED, cellSpursJobQueuePort2Sync),
        PS_HLE_FUNC(0xFC526B72, cellSpursJobQueuePort2PushSync),
        PS_HLE_FUNC(0xFF03CC79, cellSpursJobQueueAttributeSetIsHaltOnError),
        PS_HLE_FUNC(0xFF7A7BD2, cellSpursJobQueueOpen),
        PS_HLE_INTERNAL(0xEBE3028B, _pspocket_cellSpursJqExceptionHandler),
    });
}

} // namespace pspocket::hle
