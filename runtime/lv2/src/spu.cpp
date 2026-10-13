// sys_spu: SPU images, thread groups and threads (D-167), and the SPU side of them. Reference: RPCS3
// rpcs3/Emu/Cell/lv2/sys_spu.h/.cpp (the syscalls, image loading and deploying, the SPU limits) and
// rpcs3/Emu/Cell/SPUThread.cpp (stop_and_signal: stop codes 0x001, 0x002, 0x100, 0x101, 0x102, 0x110, 0x111;
// set_ch_value SPU_WrOutIntrMbox: send_event, throw_event, event flag bits; cpu_task's end of a thread's run).
//
// Each SPU thread has a host thread of its own (8 MB stack, D-015), created by sys_spu_thread_initialize and parked
// between runs. sys_spu_thread_group_start copies each thread's image into its local store, resets its context
// (ps_spu_thread_reset: RPCS3 spu_thread::cpu_init), attaches the lifted program the HLE layer finds for the image
// (Kernel::set_spu_program_identifier; the interpreter otherwise) and wakes it. Stop codes and interrupt-mailbox writes
// come back here through the context's hooks. The SPURS scheduler (HLE, D-037) does not use any of this.
//
// Differences from RPCS3:
// - While a thread waits in sys_spu_thread_receive_event the group is WAITING, as in RPCS3 (terminate is refused,
//   sys_spu_thread_group_exit waits), but its other threads go on running (RPCS3 suspends them until the event).
// - Suspending stops the threads at their next look at the attention flag (lifted code: loop back edges and runtime
//   calls), not at once.
// - SPU thread local stores are host memory: the PPU reaches them through sys_spu_thread_read_ls/write_ls, the other
//   threads of the group through the 0xF0000000 DMA window (spu_num below 8), not through guest addresses.
// - No raw or isolated SPUs, no SPU page faults, no cooperative victims (accepted, a TODO in RPCS3 too), no SPU
//   printf server here; a halt, an SPU exception or an unknown stop code ends the thread with an error log
//   (abort while the SPU miss policy is "abort": dev builds, CLAUDE.md rule 5); the exception event is not sent.

#include "spu.hpp"

#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <thread>

#include "pspocket/core/be.h"
#include "pspocket/spu/fp.h"
#include "pspocket/spu/thread.hpp"

namespace pspocket::lv2 {

namespace {

constexpr std::string_view kSpuChan = "spu";
constexpr u32 kLsSize = 0x40000;
constexpr usize kHostStack = 8u << 20;

// ------------------------------------------------------------------------------------------------- images

struct ElfPhdr {
    u32 type = 0, offset = 0, vaddr = 0, filesz = 0, memsz = 0;
};

// An SPU ELF's entry and program headers (32-bit, big-endian, EM_SPU), or nothing.
std::optional<std::pair<u32, std::vector<ElfPhdr>>> parse_spu_elf(std::span<const u8> f)
{
    if (f.size() < 0x34 || std::memcmp(f.data(), "\x7F" "ELF", 4) != 0 || f[4] != 1 || f[5] != 2) return std::nullopt;
    if (ps_ld16(f.data() + 0x12) != 23) return std::nullopt; // EM_SPU
    const u32 entry = ps_ld32(f.data() + 0x18), phoff = ps_ld32(f.data() + 0x1C);
    const u16 phentsize = ps_ld16(f.data() + 0x2A), phnum = ps_ld16(f.data() + 0x2C);
    if (phnum && (phentsize != 0x20 || phoff > f.size() || u64{phnum} * 0x20 > f.size() - phoff)) return std::nullopt;
    std::vector<ElfPhdr> ph(phnum);
    for (u32 i = 0; i < phnum; i++) {
        const u8* p = f.data() + phoff + 0x20 * i;
        ph[i] = {ps_ld32(p), ps_ld32(p + 4), ps_ld32(p + 8), ps_ld32(p + 16), ps_ld32(p + 20)};
    }
    return std::make_pair(entry, std::move(ph));
}

// RPCS3 sys_spu_image::get_nsegs: a COPY (and a FILL when memsz exceeds filesz) per PT_LOAD, an INFO per PT_NOTE;
// -1 for any other program type.
s32 count_segments(const std::vector<ElfPhdr>& ph)
{
    s32 n = 0;
    for (const ElfPhdr& p : ph) {
        if (p.type != 1 && p.type != 4) return -1;
        n += p.type == 1 && p.filesz != p.memsz && p.filesz ? 2 : 1;
    }
    return n;
}

// sys_spu_segment: the address or fill value is the first word of a doubleword-aligned union at +0x10 (RPCS3
// sys_spu.h; liblv2's raw SPU loader and libsre's cellOvis read it there), +0xC is padding.
void write_segment(Guest& g, u32 at, const SpuSegment& s)
{
    g.w32(at, static_cast<u32>(s.type));
    g.w32(at + 4, s.ls);
    g.w32(at + 8, s.size);
    g.w32(at + 12, 0);
    g.w32(at + 16, s.addr);
    g.w32(at + 20, 0);
}

SpuSegment read_segment(const Guest& g, u32 at)
{
    return {static_cast<s32>(g.r32(at)), g.r32(at + 4), g.r32(at + 8), g.r32(at + 16)};
}

// RPCS3 sys_spu_image::fill.
s32 fill_segments(Guest& g, u32 segs, s32 nsegs, const std::vector<ElfPhdr>& ph, u32 src)
{
    s32 n = 0;
    for (const ElfPhdr& p : ph) {
        if (p.type == 1) {
            if (p.filesz) {
                if (n >= nsegs) return -2;
                write_segment(g, segs + 0x18 * static_cast<u32>(n++), {SYS_SPU_SEGMENT_TYPE_COPY, p.vaddr, p.filesz, p.offset + src});
            }
            if (p.memsz > p.filesz) {
                if (n >= nsegs) return -2;
                write_segment(g, segs + 0x18 * static_cast<u32>(n++),
                    {SYS_SPU_SEGMENT_TYPE_FILL, p.vaddr + p.filesz, p.memsz - p.filesz, 0});
            }
        } else if (p.type == 4) {
            if (n >= nsegs) return -2;
            write_segment(g, segs + 0x18 * static_cast<u32>(n++), {SYS_SPU_SEGMENT_TYPE_INFO, 0, 0x20, p.offset + 0x14 + src});
        } else {
            return -1;
        }
    }
    return n;
}

// RPCS3 sys_spu_image::load: the segment table and a copy of the whole file in user memory, an image object, and the
// guest's sys_spu_image {type KERNEL, entry_point = the image's ID, segs 0, nsegs 0}.
Result load_image(Kernel::Impl& s, std::span<const u8> file, u32 img, std::string_view what)
{
    const auto elf = parse_spu_elf(file);
    if (!elf) {
        PS_LOG_ERROR(kSpuChan, "{}: not an SPU ELF ({} bytes)", what, file.size());
        return err(CELL_ENOEXEC);
    }
    const s32 nsegs = count_segments(elf->second);
    if (nsegs <= 0 || nsegs > 0x20) {
        PS_LOG_ERROR(kSpuChan, "{}: {} segments (program types other than LOAD and NOTE?)", what, nsegs);
        return err(CELL_ENOEXEC);
    }
    const u64 mem_size = 0x18ull * static_cast<u64>(nsegs) + file.size();
    u32 segs = 0;
    {
        std::lock_guard lock(s.memory.mutex);
        segs = s.memory.allocate_kernel(mem_size);
    }
    if (!segs) return err(CELL_ENOMEM);
    const u32 src = segs + 0x18 * static_cast<u32>(nsegs);
    std::memcpy(s.guest.ptr(src), file.data(), file.size());
    if (fill_segments(s.guest, segs, nsegs, elf->second, src) != nsegs) {
        std::lock_guard lock(s.memory.mutex);
        s.memory.free_kernel(segs);
        return err(CELL_ENOEXEC);
    }
    auto image = std::make_shared<SpuImage>(elf->first, segs, nsegs, static_cast<u32>(mem_size));
    const u32 id = s.objects.insert(SpuImage::kIdBase, image);
    if (!id) {
        std::lock_guard lock(s.memory.mutex);
        s.memory.free_kernel(segs);
        return err(CELL_EAGAIN);
    }
    image->id = id;
    s.guest.w32(img, SYS_SPU_IMAGE_TYPE_KERNEL);
    s.guest.w32(img + 4, id);
    s.guest.w32(img + 8, 0);
    s.guest.w32(img + 12, 0);
    PS_LOG_INFO(kSpuChan, "{}: SPU image {:#x}, entry {:#x}, {} segments", what, id, elf->first, nsegs);
    return CELL_OK;
}

// A file that starts "SCE\0" through the HLE layer's decrypter, as RPCS3's decrypt_self.
Result decrypt_if_self(Kernel::Impl& s, std::vector<u8>& bytes, std::string_view what)
{
    if (bytes.size() < 4 || std::memcmp(bytes.data(), "SCE\0", 4) != 0) return CELL_OK;
    Kernel::SelfDecrypter decrypt;
    {
        std::lock_guard lock(s.spu_hooks_mutex);
        decrypt = s.self_decrypter;
    }
    std::string error = "no SELF decrypter in this process";
    if (!decrypt || !decrypt(bytes, error)) {
        PS_LOG_ERROR(kSpuChan, "{}: an SPU SELF that does not decrypt: {}", what, error);
        return err(CELL_ENOEXEC);
    }
    return CELL_OK;
}

// RPCS3 sys_spu_image::deploy: COPY segments from guest memory, FILL segments with their 32-bit value; the LS address
// and size taken modulo the local store, as RPCS3 does to stay inside it.
void deploy(Kernel::Impl& s, u8* ls, const std::vector<SpuSegment>& segs, u32 thread_id)
{
    for (const SpuSegment& seg : segs) {
        const u32 at = seg.ls % kLsSize;
        u32 size = std::min<u32>(kLsSize - at, seg.size % kLsSize);
        if (seg.type == SYS_SPU_SEGMENT_TYPE_COPY) {
            if ((seg.ls | seg.size) % 4) PS_LOG_ERROR(kSpuChan, "SPU thread {:#x}: unaligned COPY segment ({:#x}, {:#x})", thread_id, seg.ls, seg.size);
            if (at != seg.ls || size != seg.size) PS_LOG_ERROR(kSpuChan, "SPU thread {:#x}: COPY segment ({:#x}, {:#x}) outside the local store", thread_id, seg.ls, seg.size);
            if (!s.guest.valid(seg.addr, size)) {
                PS_LOG_ERROR(kSpuChan, "SPU thread {:#x}: COPY segment from {:#x} ({:#x} bytes) is not guest memory", thread_id, seg.addr, size);
                continue;
            }
            std::memcpy(ls + at, s.guest.ptr(seg.addr), size);
        } else if (seg.type == SYS_SPU_SEGMENT_TYPE_FILL) {
            if ((seg.ls | seg.size) % 4) PS_LOG_ERROR(kSpuChan, "SPU thread {:#x}: unaligned FILL segment ({:#x}, {:#x})", thread_id, seg.ls, seg.size);
            for (u32 i = 0; i + 4 <= size; i += 4) ps_st32(ls + at + i, seg.addr);
        }
    }
}

// ------------------------------------------------------------------------------------------------- lookups

std::shared_ptr<SpuThreadGroup> get_group(Kernel::Impl& s, u32 id) { return s.spu_groups.get<SpuThreadGroup>(id); }

// RPCS3 lv2_spu_group::get_thread: bits 0-23 of a thread ID are its group's ID without the base, bits 24-31 its index.
std::pair<std::shared_ptr<SpuThreadGroup>, std::shared_ptr<SpuThread>> get_thread(Kernel::Impl& s, u32 id)
{
    if (id >= 0x06000000) return {};
    auto group = get_group(s, (id & 0xFFFFFF) | (SpuThreadGroup::kIdBase & ~0xFFFFFFu));
    if (!group) return {};
    std::lock_guard lock(group->lock);
    const u32 index = id >> 24;
    if (index >= group->init || !group->threads[index]) return {group, nullptr};
    return {group, group->threads[index]};
}

bool waiting(SpuGroupState st) { return st == SpuGroupState::Waiting || st == SpuGroupState::WaitingAndSuspended; }
bool suspended(SpuGroupState st) { return st == SpuGroupState::Suspended || st == SpuGroupState::WaitingAndSuspended; }

bool alive(const std::shared_ptr<EventQueue>& q)
{
    if (!q) return false;
    std::lock_guard lk(q->lock);
    return !q->dead;
}

// RPCS3 spu_limits_t::check_valid / check_busy (no raw SPUs here). With spu_limits.mutex held.
Result check_limits(Kernel::Impl& s, u32 physical, u32 controllable, bool coop)
{
    const u32 spu_limit = s.spu_limits.max_spu, raw_limit = s.spu_limits.max_raw;
    if (spu_limit + raw_limit > 6 || physical > spu_limit || controllable > spu_limit) return err(CELL_EINVAL);
    u32 system_coop = coop ? 1 : 0;
    for (const auto& [id, obj] : s.spu_groups.list(SpuThreadGroup::kIdBase)) {
        const auto& g = static_cast<const SpuThreadGroup&>(*obj);
        if (g.type & SYS_SPU_THREAD_GROUP_TYPE_COOPERATE_WITH_SYSTEM) {
            system_coop++;
            controllable = std::max<u32>(controllable, 1);
            physical += g.max_num - 1;
        } else if (g.has_scheduler_context) {
            controllable = std::max<u32>(controllable, g.max_num);
        } else {
            physical += g.max_num;
        }
    }
    // physical >= spu_limit is EBUSY, not EINVAL (RPCS3).
    if (physical >= spu_limit || controllable > spu_limit || system_coop > 1) return err(CELL_EBUSY);
    return CELL_OK;
}

// ------------------------------------------------------------------------------------------------- SPU side

bool abort_on_failure() { return ps_spu_get_miss_policy() == PS_SPU_MISS_ABORT; }

// An answer in the SPU's inbound mailbox; more than one word, or one that follows an action another thread may answer
// in turn, with th.answer_mutex held (D-584).
void put_in_mbox(SpuThread& th, std::initializer_list<u32> values)
{
    for (const u32 v : values) ps_spu_write_in_mbox(&th.ctx, v);
}

// 0x110 sys_spu_thread_receive_event (wait) and 0x111 sys_spu_thread_tryreceive_event: the SPU queue number is in
// the outbound mailbox; the answer goes to the inbound mailbox: CELL_OK and the event's data words, or an error.
int receive_event(SpuThread& th, bool wait)
{
    const char* what = wait ? "sys_spu_thread_receive_event" : "sys_spu_thread_tryreceive_event";
    u32 spuq = 0;
    if (!ps_spu_read_out_mbox(&th.ctx, &spuq)) {
        PS_LOG_ERROR(kSpuChan, "SPU thread {:#x}: {} with an empty outbound mailbox at {:#07x}", th.id, what, th.ctx.pc);
        return 0;
    }
    if (const u32 count = ps_spu_in_mbox_count(&th.ctx)) {
        PS_LOG_ERROR(kSpuChan, "SPU thread {:#x}: {}: the inbound mailbox holds {} entries", th.id, what, count);
        put_in_mbox(th, {static_cast<u32>(err(CELL_EBUSY))});
        return 1;
    }
    SpuThreadGroup& g = th.group;
    if (wait && !g.has_scheduler_context) {
        PS_LOG_ERROR(kSpuChan, "SPU thread {:#x}: {} in a group of type {:#x}", th.id, what, g.type);
        put_in_mbox(th, {static_cast<u32>(err(CELL_EINVAL))});
        return 1;
    }
    std::shared_ptr<EventQueue> q;
    {
        std::lock_guard lock(g.lock);
        for (const auto& [num, queue] : th.spuq)
            if (num == spuq && alive(queue)) q = queue;
    }
    if (!q) {
        put_in_mbox(th, {static_cast<u32>(err(CELL_EINVAL))});
        return 1;
    }
    Event ev;
    if (!wait) {
        if (!alive(q)) {
            put_in_mbox(th, {static_cast<u32>(err(CELL_EINVAL))});
        } else if (q->try_receive(&ev, 1)) {
            std::lock_guard answer(th.answer_mutex); // the four words as one (RPCS3: ch_in_mbox.set_values(4, ...))
            put_in_mbox(th, {CELL_OK, static_cast<u32>(ev.data1), static_cast<u32>(ev.data2), static_cast<u32>(ev.data3)});
        } else {
            put_in_mbox(th, {static_cast<u32>(err(CELL_EBUSY))});
        }
        return 1;
    }
    {
        // A terminate or group exit that came first ends the thread here (RPCS3 checks the thread's stop flag under the
        // queue's and group's locks): once the group is WAITING, terminate refuses it and group exit waits.
        std::lock_guard lock(g.lock);
        if (g.set_terminate || th.s.exiting.load(std::memory_order_relaxed)) return 0;
        if (g.state == SpuGroupState::Running) g.state = SpuGroupState::Waiting;
        else if (g.state == SpuGroupState::Suspended) g.state = SpuGroupState::WaitingAndSuspended;
    }
    th.waiter.prio.store(g.prio, std::memory_order_relaxed);
    const Result r = q->receive(th.waiter, 0, ev);
    {
        std::lock_guard lock(g.lock);
        if (g.state == SpuGroupState::Waiting) g.state = SpuGroupState::Running;
        else if (g.state == SpuGroupState::WaitingAndSuspended) g.state = SpuGroupState::Suspended;
        g.state_cv.notify_all();
    }
    if (r == CELL_OK) {
        std::lock_guard answer(th.answer_mutex);
        put_in_mbox(th, {CELL_OK, static_cast<u32>(ev.data1), static_cast<u32>(ev.data2), static_cast<u32>(ev.data3)});
        return 1;
    }
    if (r == err(CELL_EABORT)) return 0; // the process is ending
    put_in_mbox(th, {static_cast<u32>(r == err(CELL_ESRCH) ? err(CELL_EINVAL) : r)});
    return 1;
}

// 0x101 sys_spu_thread_group_exit: the status is in the outbound mailbox. Not while a thread of the group waits on an
// SPU queue; whoever ends the group first decides its cause and status (RPCS3).
int group_exit(SpuThread& th)
{
    u32 value = 0;
    if (!ps_spu_read_out_mbox(&th.ctx, &value)) {
        PS_LOG_ERROR(kSpuChan, "SPU thread {:#x}: sys_spu_thread_group_exit with an empty outbound mailbox", th.id);
        return 0;
    }
    SpuThreadGroup& g = th.group;
    std::unique_lock lock(g.lock);
    g.state_cv.wait(lock, [&] { return !waiting(g.state) || th.s.exiting.load(std::memory_order_relaxed); });
    if (std::exchange(g.set_terminate, true)) return 0;
    for (const auto& t : g.threads)
        if (t && t.get() != &th) ps_spu_request_stop(&t->ctx);
    g.exit_status = static_cast<s32>(value);
    g.join_state = SYS_SPU_THREAD_GROUP_JOIN_GROUP_EXIT;
    g.state_cv.notify_all();
    return 0;
}

int spu_stop(SPUContext* ctx, uint32_t code)
{
    SpuThread& th = *static_cast<SpuThread*>(ctx->runtime);
    switch (code) {
    case 0x001: // RPCS3: a hack that waits a little
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return 1;
    case 0x002: return 1;
    case 0x100: // sys_spu_thread_group_yield: nothing to do (RPCS3)
        if (th.ctx.ch.out_mbox_count) PS_LOG_ERROR(kSpuChan, "SPU thread {:#x}: yield (0x100) with a full outbound mailbox", th.id);
        return 1;
    case 0x101: return group_exit(th);
    case 0x102: { // sys_spu_thread_exit
        u32 value = 0;
        if (!ps_spu_read_out_mbox(&th.ctx, &value)) {
            PS_LOG_ERROR(kSpuChan, "SPU thread {:#x}: sys_spu_thread_exit with an empty outbound mailbox", th.id);
            return 0;
        }
        th.last_exit_status = static_cast<s32>(value);
        th.stopped_by_exit = true;
        return 0;
    }
    case 0x110: return receive_event(th, true);
    case 0x111: return receive_event(th, false);
    default:
        PS_LOG_ERROR(kSpuChan, "SPU thread {:#x} \"{}\": unknown stop code {:#x} near {:#07x}", th.id, th.name, code, ctx->pc);
        if (abort_on_failure()) PS_FATAL(kSpuChan, "SPU thread {:#x} stopped with code {:#x}", th.id, code);
        return 0;
    }
}

// SPU_WrOutIntrMbox (RPCS3 set_ch_value): codes 0..63 sys_spu_thread_send_event (answered in the inbound mailbox),
// 64..127 throw_event (not answered), 128 sys_event_flag_set_bit (answered) and 192 its impatient form; the data word
// (or the event flag's ID) is in the outbound mailbox.
int spu_intr_mbox(SPUContext* ctx, uint32_t value)
{
    SpuThread& th = *static_cast<SpuThread*>(ctx->runtime);
    const u32 code = value >> 24;
    u32 data = 0;
    if (!ps_spu_read_out_mbox(ctx, &data)) {
        PS_LOG_ERROR(kSpuChan, "SPU thread {:#x}: interrupt mailbox code {:#x} with an empty outbound mailbox", th.id, code);
        if (abort_on_failure()) PS_FATAL(kSpuChan, "SPU thread {:#x} stops: a malformed lv2 call", th.id);
        return 0;
    }
    if (code == 128 || code == 192) {
        const u32 bit = value & 0xFFFFFF;
        Result r = err(CELL_ESRCH);
        {
            // A PPU thread the bit wakes may answer the SPU at once: the answer goes in first (D-584).
            std::unique_lock answer(th.answer_mutex, std::defer_lock);
            if (code == 128) answer.lock();
            if (bit > 63) {
                r = err(CELL_EINVAL);
            } else if (const auto flag = th.s.kernel.event_flag(data)) {
                flag->set(u64{1} << bit);
                r = CELL_OK;
            }
            if (code == 128) put_in_mbox(th, {static_cast<u32>(r)});
        }
        if (r != CELL_OK) PS_LOG_WARN(kSpuChan, "SPU thread {:#x}: event flag {:#x} bit {}: {}", th.id, data, bit, error_name(r));
        return 0;
    }
    if (code >= 128) {
        PS_LOG_ERROR(kSpuChan, "SPU thread {:#x}: interrupt mailbox code {:#x} (value {:#010x}) is not an lv2 call", th.id, code,
            value);
        return 0;
    }
    const bool send = code < 64;
    const u32 port = code & 63;
    Result r = err(CELL_ENOTCONN);
    {
        // send_event: the receiver may answer the event as soon as it is queued (spu_printf's server does), so the
        // call's own answer goes in first; a PPU's sys_spu_thread_write_spu_mb waits for it (D-584).
        std::unique_lock answer(th.answer_mutex, std::defer_lock);
        if (send) answer.lock();
        if (send && ps_spu_in_mbox_count(ctx)) {
            put_in_mbox(th, {static_cast<u32>(err(CELL_EBUSY))});
            answer.unlock();
            PS_LOG_WARN(kSpuChan, "SPU thread {:#x}: sys_spu_thread_send_event(port {}): the inbound mailbox is not empty", th.id,
                port);
            return 0;
        }
        std::shared_ptr<EventQueue> q;
        {
            std::lock_guard lock(th.group.lock);
            q = th.ports[port];
        }
        if (alive(q)) r = q->send(Event{SYS_SPU_THREAD_EVENT_USER_KEY, th.id, (u64{port} << 32) | (value & 0xFFFFFF), data});
        if (send) put_in_mbox(th, {static_cast<u32>(r)});
    }
    if (r != CELL_OK) PS_LOG_WARN(kSpuChan, "SPU thread {:#x}: {} event on port {}: {}", th.id, send ? "send" : "throw", port, error_name(r));
    return 0;
}

constexpr ps_spu_hooks kHooks{spu_stop, spu_intr_mbox, 0, nullptr};

} // namespace

Result decrypt_spu_self(Kernel::Impl& s, std::vector<u8>& bytes, std::string_view what)
{
    return decrypt_if_self(s, bytes, what);
}

std::optional<u32> spu_elf_entry(std::span<const u8> file)
{
    const auto elf = parse_spu_elf(file);
    if (!elf || count_segments(elf->second) <= 0) return std::nullopt;
    for (const ElfPhdr& p : elf->second) {
        if (p.type == 1 && (p.offset > file.size() || p.filesz > file.size() - p.offset)) return std::nullopt;
    }
    return elf->first;
}

// RPCS3 sys_spu_image::load + deploy for one local store: each PT_LOAD's file bytes, then zeros to its memory size.
bool load_spu_elf(std::span<const u8> file, u8* ls, u32& entry)
{
    const auto e = spu_elf_entry(file);
    if (!e) return false;
    for (const ElfPhdr& p : parse_spu_elf(file)->second) {
        if (p.type != 1) continue;
        const u32 at = p.vaddr % kLsSize;
        const u32 copy = std::min<u32>(kLsSize - at, p.filesz);
        std::memcpy(ls + at, file.data() + p.offset, copy);
        if (p.memsz > p.filesz) std::memset(ls + at + copy, 0, std::min<u32>(kLsSize - at - copy, p.memsz - p.filesz));
    }
    entry = *e;
    return true;
}

// ------------------------------------------------------------------------------------------------- SpuThread

SpuThread::SpuThread(Kernel::Impl& s, SpuThreadGroup& group, u32 id, u32 spu_num, std::string name, u32 option)
    : s(s), group(group), id(id), spu_num(spu_num), name(std::move(name)), option(option), host_(s.host_threads)
{
    ls = static_cast<u8*>(std::aligned_alloc(128, kLsSize));
    std::memset(ls, 0, kLsSize);
    ps_spu_context_init(&ctx, ls, s.kernel.memory().base());
    ctx.id = id;
    ctx.index = spu_num;
    ctx.runtime = this;
    ctx.hooks = &kHooks;
    ctx.group = &group.dma;
    ps_spu_host_attach(&ctx);
    waiter.id = id;
}

SpuThread::~SpuThread()
{
    quit_and_join();
    ps_spu_host_detach(&ctx);
    std::free(ls);
    delete static_cast<pthread_t*>(handle_);
}

bool SpuThread::start_host()
{
    {
        std::lock_guard lock(host_->mutex);
        host_->live++;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kHostStack);
#if defined(__APPLE__)
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INITIATED, 0);
#endif
    handle_ = new pthread_t{};
    const int rc = pthread_create(static_cast<pthread_t*>(handle_), &attr, &SpuThread::entry, this);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        std::lock_guard lock(host_->mutex);
        host_->live--;
        host_->cv.notify_all();
        PS_LOG_ERROR(kSpuChan, "SPU thread {:#x}: no host thread (error {})", id, rc);
        return false;
    }
    joinable_ = true;
    return true;
}

void SpuThread::start_run()
{
    std::lock_guard lock(run_mutex_);
    starts_++;
    run_cv_.notify_all();
}

void SpuThread::request_quit()
{
    {
        std::lock_guard lock(run_mutex_);
        quit_ = true;
    }
    run_cv_.notify_all();
    ps_spu_request_stop(&ctx);
    abort_wait(waiter);
}

void SpuThread::quit_and_join()
{
    if (!joinable_) return;
    request_quit();
    pthread_join(*static_cast<pthread_t*>(handle_), nullptr);
    joinable_ = false;
}

void* SpuThread::entry(void* arg)
{
    static_cast<SpuThread*>(arg)->body();
    return nullptr;
}

void SpuThread::body()
{
    const std::string thread_name = std::format("SPU {:x} {}", id, name);
#if defined(__APPLE__)
    pthread_setname_np(thread_name.c_str());
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), thread_name.substr(0, 15).c_str());
#endif
    const u64 fp_env = ps_spu_fpenv_enter();
    for (;;) {
        {
            std::unique_lock lock(run_mutex_);
            run_cv_.wait(lock, [&] { return quit_ || starts_ != runs_; });
            if (quit_) break;
            runs_ = starts_;
        }
        run();
    }
    ps_spu_fpenv_leave(fp_env);
    std::lock_guard lock(host_->mutex);
    host_->live--;
    host_->cv.notify_all();
}

void SpuThread::run()
{
    for (;;) {
        // A suspended group parks its threads until it is resumed; a terminate, a group exit or the process's end ends
        // the run. The stop request is cleared under the group's lock, where terminate and suspend make theirs, so
        // none is lost.
        {
            std::unique_lock lock(group.lock);
            const auto ending = [&] { return group.set_terminate || s.exiting.load(std::memory_order_relaxed); };
            group.state_cv.wait(lock, [&] { return ending() || !(suspended(group.state) || group.dbg_stopped); });
            if (ending()) break;
            ps_spu_clear_stop(&ctx);
        }
        const u32 reason = ps_spu_run(&ctx);
        if (reason == PS_SPU_STOP_REQUESTED) continue;
        if (reason == PS_SPU_STOP_SIGNAL) break; // sys_spu_thread_exit, group exit, an unknown code or the process's end
        if (reason == PS_SPU_STOP_HALT || reason == PS_SPU_STOP_EXCEPTION) {
            PS_LOG_ERROR(kSpuChan, "SPU thread {:#x} \"{}\": {} at {:#07x} (detail {:#x}); the thread stops", id, name,
                reason == PS_SPU_STOP_HALT ? "halt" : std::format("exception {}", ctx.exception), ctx.pc, ctx.stop_code);
            if (abort_on_failure()) PS_FATAL(kSpuChan, "SPU thread {:#x} halted or faulted", id);
            break;
        }
        PS_LOG_ERROR(kSpuChan, "SPU thread {:#x}: stopped (reason {}) at {:#07x}", id, reason, ctx.pc);
        break;
    }
    stopped();
}

// RPCS3 spu_thread::cpu_task's end: the inbound mailbox emptied; the thread's exit status kept when it called
// sys_spu_thread_exit; the last thread of the run sets the group back to INITIALIZED, its cause (all threads exited,
// unless a group exit or terminate set one), counts the stop and answers a joiner.
void SpuThread::stopped()
{
    ps_spu_clear_in_mbox(&ctx);
    std::lock_guard lock(group.lock);
    if (stopped_by_exit) {
        exit_status = last_exit_status;
        exit_status_valid = true;
    }
    if (group.running && --group.running == 0) {
        group.state = SpuGroupState::Initialized;
        if (!group.join_state) group.join_state = SYS_SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT;
        group.stop_count++;
        if (Waiter* w = std::exchange(group.joiner, nullptr)) {
            w->data[0] = group.join_state;
            w->data[1] = static_cast<u32>(group.exit_status);
            group.join_state = 0;
            wake(*w, CELL_OK);
        }
        PS_LOG_DEBUG(kSpuChan, "SPU thread group {:#x} stopped (count {})", group.id, group.stop_count);
    }
    group.state_cv.notify_all();
}

void abort_spu_threads(Kernel::Impl& s)
{
    for (const auto& [id, obj] : s.spu_groups.list(SpuThreadGroup::kIdBase)) {
        auto& g = static_cast<SpuThreadGroup&>(*obj);
        std::vector<std::shared_ptr<SpuThread>> threads;
        {
            std::lock_guard lock(g.lock);
            for (const auto& t : g.threads)
                if (t) threads.push_back(t);
            g.state_cv.notify_all();
        }
        for (const auto& t : threads) t->request_quit();
    }
}

// ------------------------------------------------------------------------------------------------- syscalls

Result sys_spu_initialize(PpuThread& t, u32 max_usable_spu, u32 max_raw_spu)
{
    if (max_raw_spu > 5) return err(CELL_EINVAL);
    max_usable_spu = 6; // RPCS3: "This value can be changed by VSH in theory"
    Kernel::Impl& s = t.kernel.impl();
    std::lock_guard lock(s.spu_limits.mutex);
    // RPCS3 checks the new split against the groups that exist (check_busy).
    const u32 old_spu = s.spu_limits.max_spu, old_raw = s.spu_limits.max_raw;
    s.spu_limits.max_raw = max_raw_spu;
    s.spu_limits.max_spu = max_usable_spu - max_raw_spu;
    if (s.spu_groups.count(SpuThreadGroup::kIdBase) && check_limits(s, 0, 0, false) != CELL_OK) {
        s.spu_limits.max_raw = old_raw;
        s.spu_limits.max_spu = old_spu;
        return err(CELL_EBUSY);
    }
    PS_LOG_INFO("lv2", "sys_spu_initialize: {} SPUs for thread groups, {} raw SPUs", s.spu_limits.max_spu, max_raw_spu);
    return CELL_OK;
}

// sys_spu_thread_group_log(command, stat), as RPCS3: the process-wide SPU thread group log switch. 0 turns it on
// (the default), 1 off, 2 reads it into *stat (CELL_EFAULT without one); anything else is CELL_EINVAL. Nothing is
// logged either way: the switch only answers what it was set to.
Result sys_spu_thread_group_log(PpuThread& t, s32 command, u32 stat)
{
    constexpr s32 kOn = 0, kOff = 1, kGetStatus = 2;
    Kernel::Impl& s = t.kernel.impl();
    std::lock_guard lock(s.spu_limits.mutex);
    switch (command) {
    case kGetStatus:
        if (!stat || !s.guest.valid(stat, 4)) return err(CELL_EFAULT);
        s.guest.w32(stat, static_cast<u32>(s.spu_limits.group_log));
        return CELL_OK;
    case kOn:
    case kOff:
        s.spu_limits.group_log = command;
        return CELL_OK;
    default: return err(CELL_EINVAL);
    }
}

Result _sys_spu_image_get_information(PpuThread& t, u32 img, u32 entry_point, u32 nsegs)
{
    Kernel::Impl& s = t.kernel.impl();
    if (!s.guest.valid(img, 16) || !s.guest.valid(entry_point, 4) || !s.guest.valid(nsegs, 4)) return err(CELL_EFAULT);
    if (s.guest.r32(img) != SYS_SPU_IMAGE_TYPE_KERNEL) return err(CELL_EINVAL);
    const auto image = s.objects.get<SpuImage>(s.guest.r32(img + 4));
    if (!image) return err(CELL_ESRCH);
    s.guest.w32(entry_point, image->entry);
    s.guest.w32(nsegs, static_cast<u32>(image->nsegs));
    return CELL_OK;
}

Result sys_spu_image_open(PpuThread& t, u32 img, u32 path)
{
    Kernel::Impl& s = t.kernel.impl();
    if (!s.guest.valid(img, 16)) return err(CELL_EFAULT);
    std::string p;
    for (u32 i = 0; i < 1024; i++) {
        if (!s.guest.valid(path + i, 1)) return err(CELL_EFAULT);
        const char c = static_cast<char>(s.guest.r8(path + i));
        if (!c) break;
        p += c;
    }
    std::vector<u8> bytes;
    if (!t.kernel.vfs_read_file(p, bytes)) {
        PS_LOG_ERROR(kSpuChan, "sys_spu_image_open({}): no such file", p);
        return err(CELL_ENOENT);
    }
    if (const Result r = decrypt_if_self(s, bytes, p); r != CELL_OK) return r;
    return load_image(s, bytes, img, std::format("sys_spu_image_open({})", p));
}

Result sys_spu_image_open_by_fd(PpuThread& t, u32 img, s32 fd, s64 offset)
{
    Kernel::Impl& s = t.kernel.impl();
    if (!s.guest.valid(img, 16)) return err(CELL_EFAULT);
    std::vector<u8> bytes;
    if (const Result r = fs_read_whole_file(t.kernel, static_cast<u32>(fd), bytes); r != CELL_OK) return r;
    if (offset < 0) return err(CELL_ENOEXEC);
    if (static_cast<u64>(offset) > bytes.size()) return err(CELL_ENOEXEC);
    bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    if (const Result r = decrypt_if_self(s, bytes, "sys_spu_image_open_by_fd"); r != CELL_OK) return r;
    return load_image(s, bytes, img, std::format("sys_spu_image_open_by_fd({}, {:#x})", fd, offset));
}

// _sys_spu_image_import(img, src, size, arg4): an SPU ELF in guest memory (RPCS3 ignores the result of the load).
Result _sys_spu_image_import(PpuThread& t, u32 img, u32 src, u32 size, u32 arg4)
{
    Kernel::Impl& s = t.kernel.impl();
    if (!s.guest.valid(img, 16) || !s.guest.valid(src, size)) return err(CELL_EFAULT);
    const std::vector<u8> bytes(s.guest.ptr(src), s.guest.ptr(src) + size);
    const Result r = load_image(s, bytes, img, std::format("_sys_spu_image_import({:#x}, {:#x}, {:#x})", src, size, arg4));
    if (r != CELL_OK) PS_LOG_ERROR(kSpuChan, "_sys_spu_image_import: {} (CELL_OK, as RPCS3)", error_name(r));
    return CELL_OK;
}

Result _sys_spu_image_close(PpuThread& t, u32 img)
{
    Kernel::Impl& s = t.kernel.impl();
    if (!s.guest.valid(img, 16)) return err(CELL_EFAULT);
    if (s.guest.r32(img) != SYS_SPU_IMAGE_TYPE_KERNEL) return err(CELL_EINVAL);
    const u32 id = s.guest.r32(img + 4);
    const auto image = s.objects.get<SpuImage>(id);
    if (!image || !s.objects.remove(SpuImage::kIdBase, id)) return err(CELL_ESRCH);
    std::lock_guard lock(s.memory.mutex);
    s.memory.free_kernel(image->segs);
    return CELL_OK;
}

Result _sys_spu_image_get_segments(PpuThread& t, u32 img, u32 segments, s32 nseg)
{
    Kernel::Impl& s = t.kernel.impl();
    if (!s.guest.valid(img, 16)) return err(CELL_EFAULT);
    if (nseg <= 0 || nseg > 0x20 || s.guest.r32(img) != SYS_SPU_IMAGE_TYPE_KERNEL) return err(CELL_EINVAL);
    const auto image = s.objects.get<SpuImage>(s.guest.r32(img + 4));
    if (!image) return err(CELL_ESRCH);
    const u32 n = static_cast<u32>(std::min<s32>(nseg, image->nsegs));
    if (!s.guest.valid(segments, 0x18 * n)) return err(CELL_EFAULT);
    std::memmove(s.guest.ptr(segments), s.guest.ptr(image->segs), 0x18 * n);
    return CELL_OK;
}

Result sys_spu_thread_group_create(PpuThread& t, u32 id_out, u32 num, s32 prio, u32 attr)
{
    Kernel::Impl& s = t.kernel.impl();
    if (!s.guest.valid(id_out, 4) || !s.guest.valid(attr, 12)) return err(CELL_EFAULT);
    const bool root = t.kernel.config().root;
    const s32 min_prio = root ? 0 : 16;
    const u32 nsize = s.guest.r32(attr), name_addr = s.guest.r32(attr + 4);
    const s32 type = static_cast<s32>(s.guest.r32(attr + 8));
    // The container ID at offset 12 is read only for groups that take their memory from one (as lv2 does).
    u32 ct_id = 0;
    if (type & SYS_SPU_THREAD_GROUP_TYPE_MEMORY_FROM_CONTAINER) {
        if (!s.guest.valid(attr + 12, 4)) return err(CELL_EFAULT);
        ct_id = s.guest.r32(attr + 12);
    }
    if (nsize > 0x80 || !num) return err(CELL_EINVAL);
    std::string name;
    if (nsize) {
        if (!s.guest.valid(name_addr, nsize - 1)) return err(CELL_EFAULT);
        for (u32 i = 0; i + 1 < nsize; i++) {
            const char c = static_cast<char>(s.guest.r8(name_addr + i));
            if (!c) break;
            name += c;
        }
    }
    bool scheduled = true, needs_root = false;
    u32 max_threads = 6, min_threads = 1;
    switch (type) {
    case 0x0:
    case SYS_SPU_THREAD_GROUP_TYPE_MEMORY_FROM_CONTAINER:
    case SYS_SPU_THREAD_GROUP_TYPE_EXCLUSIVE_NON_CONTEXT: break;
    case SYS_SPU_THREAD_GROUP_TYPE_COOPERATE_WITH_SYSTEM:
    case SYS_SPU_THREAD_GROUP_TYPE_COOPERATE_WITH_SYSTEM | 0x2:
    case SYS_SPU_THREAD_GROUP_TYPE_COOPERATE_WITH_SYSTEM | 0x4:
    case SYS_SPU_THREAD_GROUP_TYPE_COOPERATE_WITH_SYSTEM | 0x6:
        needs_root = type == 0x22 || type == 0x26;
        min_threads = 2; // one is shared with the system (the cooperative victim)
        break;
    case 0x2: case 0x6: case 0xA: case 0x102: case 0x106: case 0x10A: case 0x202: case 0x206: case 0x20A: case 0x902:
    case 0x906: case 0xA02: case 0xA06: case 0xC02: case 0xC06:
        if (type & 0x700) max_threads = 1;
        needs_root = true;
        break;
    default: return err(CELL_EINVAL);
    }
    const bool coop = type & SYS_SPU_THREAD_GROUP_TYPE_COOPERATE_WITH_SYSTEM;
    u32 mem_size = 0;
    if (coop) {
        mem_size = kLsSize;
    } else if (type & SYS_SPU_THREAD_GROUP_TYPE_NON_CONTEXT) {
        scheduled = false;
    } else {
        mem_size = kLsSize * num; // RPCS3: probably the scheduler's save area for each local store
    }
    if (num < min_threads || num > max_threads || (needs_root && !root) ||
        (scheduled && !coop && (prio > 255 || prio < min_prio))) {
        return err(CELL_EINVAL);
    }
    std::shared_ptr<MemoryContainer> ct;
    {
        std::lock_guard lock(s.memory.mutex);
        if ((type & SYS_SPU_THREAD_GROUP_TYPE_MEMORY_FROM_CONTAINER) && mem_size) {
            ct = s.containers.get<MemoryContainer>(ct_id);
            if (!ct) return err(CELL_ESRCH);
        }
        if (!s.memory.container_or_default(ct).take(mem_size)) return err(CELL_ENOMEM);
    }
    const auto give_back = [&] {
        std::lock_guard lock(s.memory.mutex);
        s.memory.container_or_default(ct).give_back(mem_size);
    };
    std::lock_guard limits(s.spu_limits.mutex);
    const u32 physical = coop ? num - 1 : scheduled ? 0 : num, controllable = coop ? 1 : scheduled ? num : 0;
    if (const Result r = check_limits(s, physical, controllable, coop); r != CELL_OK) {
        give_back();
        return r;
    }
    auto group = std::make_shared<SpuThreadGroup>(name, num, prio, type, ct, scheduled, mem_size);
    const u32 id = s.spu_groups.insert(SpuThreadGroup::kIdBase, group);
    if (!id) {
        give_back();
        return err(CELL_EAGAIN);
    }
    group->id = id;
    group->dma.count = 8;
    s.guest.w32(id_out, id);
    PS_LOG_INFO(kSpuChan, "sys_spu_thread_group_create(\"{}\", {} threads, priority {}, type {:#x}) -> {:#x}", name, num, prio,
        type, id);
    return CELL_OK;
}

Result sys_spu_thread_group_destroy(PpuThread& t, u32 id)
{
    Kernel::Impl& s = t.kernel.impl();
    std::lock_guard limits(s.spu_limits.mutex);
    const auto g = get_group(s, id);
    if (!g) return err(CELL_ESRCH);
    std::array<std::shared_ptr<SpuThread>, 8> threads;
    {
        std::lock_guard lock(g->lock);
        if (g->state > SpuGroupState::Initialized) return err(CELL_EBUSY);
        g->state = SpuGroupState::Destroyed;
        threads = g->threads;
    }
    s.spu_groups.remove(SpuThreadGroup::kIdBase, id);
    {
        std::lock_guard lock(s.memory.mutex);
        s.memory.container_or_default(g->container).give_back(g->mem_size);
    }
    for (const auto& th : threads)
        if (th) th->quit_and_join();
    PS_LOG_DEBUG(kSpuChan, "sys_spu_thread_group_destroy({:#x})", id);
    return CELL_OK;
}

Result sys_spu_thread_initialize(PpuThread& t, u32 thread_out, u32 group_id, u32 spu_num, u32 img, u32 attr, u32 arg)
{
    Kernel::Impl& s = t.kernel.impl();
    if (spu_num >= 256) return err(CELL_EINVAL);
    if (!attr || !s.guest.valid(attr, 12)) return err(CELL_EFAULT);
    const u32 name_addr = s.guest.r32(attr), name_len = s.guest.r32(attr + 4), option = s.guest.r32(attr + 8);
    if (name_len > 0x80) return err(CELL_EINVAL);
    if (!arg || !s.guest.valid(arg, 32)) return err(CELL_EFAULT);
    const std::array<u64, 4> args{s.guest.r64(arg), s.guest.r64(arg + 8), s.guest.r64(arg + 16), s.guest.r64(arg + 24)};
    if (option & ~(SYS_SPU_THREAD_OPTION_DEC_SYNC_TB_ENABLE | SYS_SPU_THREAD_OPTION_ASYNC_INTR_ENABLE)) return err(CELL_EINVAL);
    if (!img || !s.guest.valid(img, 16) || !s.guest.valid(thread_out, 4)) return err(CELL_EFAULT);
    u32 type = s.guest.r32(img), entry = s.guest.r32(img + 4), segs = s.guest.r32(img + 8);
    s32 nsegs = static_cast<s32>(s.guest.r32(img + 12));
    if (type == SYS_SPU_IMAGE_TYPE_KERNEL) {
        const auto image = s.objects.get<SpuImage>(entry);
        if (!image) return err(CELL_ESRCH);
        entry = image->entry;
        segs = image->segs;
        nsegs = image->nsegs;
    } else if (type == SYS_SPU_IMAGE_TYPE_USER) {
        if (entry > 0x3FFFC || nsegs <= 0 || nsegs > 0x20) return err(CELL_EINVAL);
    } else {
        return err(CELL_EINVAL);
    }
    if (!s.guest.valid(segs, 0x18 * static_cast<u32>(nsegs))) return err(CELL_EFAULT);
    std::vector<SpuSegment> seg_list(static_cast<usize>(nsegs));
    for (s32 i = 0; i < nsegs; i++) seg_list[static_cast<usize>(i)] = read_segment(s.guest, segs + 0x18 * static_cast<u32>(i));
    // A user image's segments are checked (a kernel image's come from lv2): COPY 4-aligned, LS address and size
    // 16-aligned and inside the local store, no overlaps, at most one INFO of 256 bytes, at least one COPY.
    bool found_copy = type == SYS_SPU_IMAGE_TYPE_KERNEL, found_info = false;
    std::vector<u8> info;
    for (usize i = 0; type == SYS_SPU_IMAGE_TYPE_USER && i < seg_list.size(); i++) {
        const SpuSegment& seg = seg_list[i];
        switch (seg.type) {
        case SYS_SPU_SEGMENT_TYPE_COPY:
            if (seg.addr % 4) return err(CELL_EINVAL);
            found_copy = true;
            break;
        case SYS_SPU_SEGMENT_TYPE_FILL: break;
        case SYS_SPU_SEGMENT_TYPE_INFO:
            if (seg.size > 256 || found_info) return err(CELL_EINVAL);
            found_info = true;
            // The kernel keeps a copy of its bytes (the debugging calls' SPU thread information, D-937).
            if (seg.size && !s.guest.valid(seg.addr, seg.size)) return err(CELL_EFAULT);
            info.assign(s.guest.ptr(seg.addr), s.guest.ptr(seg.addr) + seg.size);
            continue;
        default: return err(CELL_EINVAL);
        }
        if (!seg.size || (seg.ls | seg.size) % 0x10 || seg.ls >= kLsSize || seg.size > kLsSize) return err(CELL_EINVAL);
        for (usize j = 0; j < i; j++) {
            const SpuSegment& o = seg_list[j];
            if (o.type != SYS_SPU_SEGMENT_TYPE_INFO && u64{seg.ls} + seg.size > o.ls && u64{o.ls} + o.size > seg.ls) {
                return err(CELL_EINVAL);
            }
        }
    }
    if (!found_copy) return err(CELL_EINVAL);
    std::string name;
    if (name_len) {
        if (!s.guest.valid(name_addr, name_len - 1)) return err(CELL_EFAULT);
        for (u32 i = 0; i + 1 < name_len; i++) {
            const char c = static_cast<char>(s.guest.r8(name_addr + i));
            if (!c) break;
            name += c;
        }
    }
    const auto g = get_group(s, group_id);
    if (!g) return err(CELL_ESRCH);
    std::shared_ptr<SpuThread> th;
    {
        std::lock_guard lock(g->lock);
        if (g->state != SpuGroupState::NotInitialized) return err(g->state == SpuGroupState::Destroyed ? CELL_ESRCH : CELL_EBUSY);
        if (g->threads_map[spu_num] != -1) return err(CELL_EBUSY);
        const u32 index = g->init;
        const u32 tid = (index << 24) | (group_id & 0xFFFFFF);
        th = std::make_shared<SpuThread>(s, *g, tid, spu_num, name, option);
        th->info = std::move(info);
        g->threads[index] = th;
        g->threads_map[spu_num] = static_cast<s8>(index);
        if (spu_num < 8) g->dma.members[spu_num] = &th->ctx;
        g->args[index] = args;
        g->imgs[index] = {entry, std::move(seg_list)};
        if (++g->init == g->max_num) g->state = SpuGroupState::Initialized;
    }
    if (option & SYS_SPU_THREAD_OPTION_ASYNC_INTR_ENABLE) PS_LOG_WARN(kSpuChan, "SPU thread {:#x}: asynchronous interrupts are not implemented", th->id);
    if (!th->start_host()) return err(CELL_EAGAIN);
    s.guest.w32(thread_out, th->id);
    PS_LOG_INFO(kSpuChan, "sys_spu_thread_initialize: SPU thread \"{}\" {:#x} (SPU {}) in group {:#x}, entry {:#x}", name, th->id,
        spu_num, group_id, entry);
    return CELL_OK;
}

Result sys_spu_thread_set_argument(PpuThread& t, u32 id, u32 arg)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto [g, th] = get_thread(s, id);
    if (!th) return err(CELL_ESRCH);
    if (!s.guest.valid(arg, 32)) return err(CELL_EFAULT);
    std::lock_guard lock(g->lock);
    g->args[id >> 24] = {s.guest.r64(arg), s.guest.r64(arg + 8), s.guest.r64(arg + 16), s.guest.r64(arg + 24)};
    return CELL_OK;
}

Result sys_spu_thread_get_exit_status(PpuThread& t, u32 id, u32 status)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto [g, th] = get_thread(s, id);
    if (!th) return err(CELL_ESRCH);
    std::lock_guard lock(g->lock);
    if (!th->exit_status_valid) return err(CELL_ESTAT);
    if (!s.guest.valid(status, 4)) return err(CELL_EFAULT);
    s.guest.w32(status, static_cast<u32>(th->exit_status));
    return CELL_OK;
}

// sys_spu_thread_recover_page_fault(id) (198): as sys_ppu_thread_recover_page_fault (thread.cpp): no SPU thread is
// ever suspended at a page fault here, so a live one answers EINVAL. RPCS3: Emu/Cell/lv2/sys_spu.cpp. The firmware's
// AAC and ATRAC encoders issue it.
Result sys_spu_thread_recover_page_fault(PpuThread& t, u32 id)
{
    if (!get_thread(t.kernel.impl(), id).second) return err(CELL_ESRCH);
    return err(CELL_EINVAL);
}

Result sys_spu_thread_group_start(PpuThread& t, u32 id)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto g = get_group(s, id);
    if (!g) return err(CELL_ESRCH);
    Kernel::SpuProgramIdentifier identify;
    {
        std::lock_guard lock(s.spu_hooks_mutex);
        identify = s.spu_identifier;
    }
    std::vector<std::shared_ptr<SpuThread>> run;
    {
        std::lock_guard lock(g->lock);
        if (g->state != SpuGroupState::Initialized) return err(g->state == SpuGroupState::Destroyed ? CELL_ESRCH : CELL_ESTAT);
        g->state = SpuGroupState::Running;
        g->join_state = 0;
        g->exit_status = 0;
        g->running = g->max_num;
        g->set_terminate = false;
        for (u32 i = 0; i < g->max_num; i++) {
            const auto& th = g->threads[i];
            if (!th) continue;
            const auto& [entry, segs] = g->imgs[i];
            deploy(s, th->ls, segs, th->id);
            const u32 now = static_cast<u32>(ps_spu_timebase());
            ps_spu_thread_reset(&th->ctx, (th->option & SYS_SPU_THREAD_OPTION_DEC_SYNC_TB_ENABLE) ? ~now : 0);
            for (u32 a = 0; a < 4; a++) th->ctx.gpr[3 + a].u64[1] = g->args[i][a];
            th->ctx.pc = entry & 0x3FFFC;
            th->stopped_by_exit = false;
            th->exit_status_valid = false;
            if (identify) {
                for (const SpuSegment& seg : segs) {
                    if (seg.type == SYS_SPU_SEGMENT_TYPE_COPY && entry >= seg.ls && entry < u64{seg.ls} + seg.size) {
                        identify(th->ctx, seg.addr, seg.ls, seg.size & ~3u, entry);
                        break;
                    }
                }
            }
            run.push_back(th);
        }
        // READY is never entered, so the run event goes out at once (RPCS3: "TODO: check data2 and data3").
        if (alive(g->ep_run)) g->ep_run->send(Event{SYS_SPU_THREAD_GROUP_EVENT_RUN_KEY, id, 0, 0});
    }
    for (const auto& th : run) th->start_run();
    return CELL_OK;
}

Result sys_spu_thread_group_suspend(PpuThread& t, u32 id)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto g = get_group(s, id);
    if (!g) return err(CELL_ESRCH);
    if (!g->has_scheduler_context || (g->type & 0xF00) || (g->type & SYS_SPU_THREAD_GROUP_TYPE_COOPERATE_WITH_SYSTEM)) {
        return err(CELL_EINVAL);
    }
    std::lock_guard lock(g->lock);
    switch (g->state) {
    case SpuGroupState::Destroyed: return err(CELL_ESRCH);
    case SpuGroupState::Running: g->state = SpuGroupState::Suspended; break;
    case SpuGroupState::Waiting: g->state = SpuGroupState::WaitingAndSuspended; break;
    case SpuGroupState::Suspended:
    case SpuGroupState::WaitingAndSuspended: return CELL_OK;
    default: return err(CELL_ESTAT);
    }
    for (const auto& th : g->threads)
        if (th) ps_spu_request_stop(&th->ctx);
    return CELL_OK;
}

Result sys_spu_thread_group_resume(PpuThread& t, u32 id)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto g = get_group(s, id);
    if (!g) return err(CELL_ESRCH);
    if (!g->has_scheduler_context || (g->type & 0xF00) || (g->type & SYS_SPU_THREAD_GROUP_TYPE_COOPERATE_WITH_SYSTEM)) {
        return err(CELL_EINVAL);
    }
    std::lock_guard lock(g->lock);
    switch (g->state) {
    case SpuGroupState::Destroyed: return err(CELL_ESRCH);
    case SpuGroupState::Suspended: g->state = SpuGroupState::Running; break;
    case SpuGroupState::WaitingAndSuspended: g->state = SpuGroupState::Waiting; break;
    default: return err(CELL_ESTAT);
    }
    g->state_cv.notify_all();
    return CELL_OK;
}

Result sys_spu_thread_group_yield(PpuThread& t, u32 id)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto g = get_group(s, id);
    if (!g) return err(CELL_ESRCH);
    if (!g->has_scheduler_context || (g->type & 0xF00)) return CELL_OK; // no effect on these types
    std::lock_guard lock(g->lock);
    if (g->state != SpuGroupState::Running) return err(g->state == SpuGroupState::Destroyed ? CELL_ESRCH : CELL_ESTAT);
    return CELL_OK; // READY is never entered: nothing to yield to (RPCS3)
}

Result sys_spu_thread_group_terminate(PpuThread& t, u32 id, s32 value)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto g = get_group(s, id);
    if (!g) return err(CELL_ESRCH);
    std::unique_lock lock(g->lock);
    const SpuGroupState st = g->state;
    if (st == SpuGroupState::Destroyed) return err(CELL_ESRCH);
    if (st <= SpuGroupState::Initialized || waiting(st)) return err(CELL_ESTAT);
    const auto wait_for_stop = [&](u32 last) {
        while (g->stop_count == last) {
            if (s.exiting.load(std::memory_order_relaxed)) throw ThreadExit{};
            g->state_cv.wait_for(lock, std::chrono::milliseconds(50));
        }
    };
    if (g->set_terminate) {
        // Another terminate or a group exit is under way: wait for it, then report ESTAT (RPCS3).
        wait_for_stop(g->stop_count);
        return err(CELL_ESTAT);
    }
    g->set_terminate = true;
    for (const auto& th : g->threads)
        if (th) ps_spu_request_stop(&th->ctx);
    g->exit_status = value;
    g->join_state = SYS_SPU_THREAD_GROUP_JOIN_TERMINATED;
    g->state_cv.notify_all(); // suspended threads end
    wait_for_stop(g->stop_count);
    return CELL_OK;
}

Result sys_spu_thread_group_join(PpuThread& t, u32 id, u32 cause, u32 status)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto g = get_group(s, id);
    if (!g) return err(CELL_ESRCH);
    u32 join_state = 0;
    s32 exit_status = 0;
    {
        std::unique_lock lock(g->lock);
        if (g->state == SpuGroupState::Destroyed) return err(CELL_ESRCH);
        if (g->state < SpuGroupState::Initialized) return err(CELL_ESTAT);
        if (g->joiner) return err(CELL_EBUSY); // another PPU thread is joining
        if (g->join_state && g->state == SpuGroupState::Initialized) {
            join_state = g->join_state;
            exit_status = g->exit_status;
            g->join_state = 0;
        } else {
            Waiter& w = t.waiter;
            prepare_wait(w);
            g->joiner = &w;
            const WaitStatus ws = park(w, lock, 0, {WaitKind::Join, id}, [&](bool) {
                if (g->joiner != &w) return CancelResult::Woken;
                g->joiner = nullptr;
                return CancelResult::Removed;
            });
            if (ws != WaitStatus::Woken) return finish(t, err(CELL_EABORT));
            join_state = static_cast<u32>(w.data[0]);
            exit_status = static_cast<s32>(w.data[1]);
        }
    }
    if (!cause || !s.guest.valid(cause, 4)) return err(CELL_EFAULT);
    s.guest.w32(cause, join_state);
    if (!status || !s.guest.valid(status, 4)) return err(CELL_EFAULT);
    s.guest.w32(status, static_cast<u32>(exit_status));
    return CELL_OK;
}

Result sys_spu_thread_group_set_priority(PpuThread& t, u32 id, s32 priority)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto g = get_group(s, id);
    if (!g) return err(CELL_ESRCH);
    if (!g->has_scheduler_context || priority < (t.kernel.config().root ? 0 : 16) || priority > 255) return err(CELL_EINVAL);
    if (g->type & SYS_SPU_THREAD_GROUP_TYPE_COOPERATE_WITH_SYSTEM) return err(CELL_EINVAL);
    std::lock_guard lock(g->lock);
    g->prio = priority;
    return CELL_OK;
}

Result sys_spu_thread_group_get_priority(PpuThread& t, u32 id, u32 priority)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto g = get_group(s, id);
    if (!g) return err(CELL_ESRCH);
    if (!s.guest.valid(priority, 4)) return err(CELL_EFAULT);
    std::lock_guard lock(g->lock);
    const s32 p = !g->has_scheduler_context ? 0 : (g->type & SYS_SPU_THREAD_GROUP_TYPE_COOPERATE_WITH_SYSTEM) ? 15 : g->prio;
    s.guest.w32(priority, static_cast<u32>(p));
    return CELL_OK;
}

Result sys_spu_thread_write_ls(PpuThread& t, u32 id, u32 lsa, u64 value, u32 type)
{
    Kernel::Impl& s = t.kernel.impl();
    if (lsa >= kLsSize || type > 8 || !type || ((type | lsa) & (type - 1))) return err(CELL_EINVAL);
    const auto [g, th] = get_thread(s, id);
    if (!th) return err(CELL_ESRCH);
    std::lock_guard lock(g->lock);
    if (g->state < SpuGroupState::Waiting || g->state > SpuGroupState::Running) {
        return err(g->state == SpuGroupState::Destroyed ? CELL_ESRCH : CELL_ESTAT);
    }
    u8* p = th->ls + lsa;
    switch (type) {
    case 1: *p = static_cast<u8>(value); break;
    case 2: ps_st16(p, static_cast<u16>(value)); break;
    case 4: ps_st32(p, static_cast<u32>(value)); break;
    case 8: ps_st64(p, value); break;
    default: return err(CELL_EINVAL);
    }
    return CELL_OK;
}

Result sys_spu_thread_read_ls(PpuThread& t, u32 id, u32 lsa, u32 value, u32 type)
{
    Kernel::Impl& s = t.kernel.impl();
    if (lsa >= kLsSize || type > 8 || !type || ((type | lsa) & (type - 1))) return err(CELL_EINVAL);
    const auto [g, th] = get_thread(s, id);
    if (!th) return err(CELL_ESRCH);
    u64 v = 0;
    {
        std::lock_guard lock(g->lock);
        if (g->state < SpuGroupState::Waiting || g->state > SpuGroupState::Running) {
            return err(g->state == SpuGroupState::Destroyed ? CELL_ESRCH : CELL_ESTAT);
        }
        const u8* p = th->ls + lsa;
        switch (type) {
        case 1: v = *p; break;
        case 2: v = ps_ld16(p); break;
        case 4: v = ps_ld32(p); break;
        case 8: v = ps_ld64(p); break;
        default: return err(CELL_EINVAL);
        }
    }
    if (!s.guest.valid(value, 8)) return err(CELL_EFAULT);
    s.guest.w64(value, v);
    return CELL_OK;
}

Result sys_spu_thread_write_spu_mb(PpuThread& t, u32 id, u32 value)
{
    const auto [g, th] = get_thread(t.kernel.impl(), id);
    if (!th) return err(CELL_ESRCH);
    std::lock_guard answer(th->answer_mutex); // after an answer lv2 is giving, never inside or ahead of it (D-584)
    ps_spu_write_in_mbox(&th->ctx, value);    // a full mailbox's newest entry is replaced (RPCS3)
    return CELL_OK;
}

Result sys_spu_thread_set_spu_cfg(PpuThread& t, u32 id, u64 value)
{
    if (value > 3) return err(CELL_EINVAL);
    const auto [g, th] = get_thread(t.kernel.impl(), id);
    if (!th) return err(CELL_ESRCH);
    std::lock_guard lock(g->lock);
    th->snr_config = value;
    ps_spu_set_signal_or_mode(&th->ctx, 1, (value & 1) != 0);
    ps_spu_set_signal_or_mode(&th->ctx, 2, (value & 2) != 0);
    return CELL_OK;
}

Result sys_spu_thread_get_spu_cfg(PpuThread& t, u32 id, u32 value)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto [g, th] = get_thread(s, id);
    if (!th) return err(CELL_ESRCH);
    if (!s.guest.valid(value, 8)) return err(CELL_EFAULT);
    std::lock_guard lock(g->lock);
    s.guest.w64(value, th->snr_config);
    return CELL_OK;
}

Result sys_spu_thread_write_snr(PpuThread& t, u32 id, u32 number, u32 value)
{
    if (number > 1) return err(CELL_EINVAL);
    const auto [g, th] = get_thread(t.kernel.impl(), id);
    if (!th) return err(CELL_ESRCH);
    ps_spu_write_signal(&th->ctx, number + 1, value);
    return CELL_OK;
}

Result sys_spu_thread_group_connect_event(PpuThread& t, u32 id, u32 eq, u32 et)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto g = get_group(s, id);
    if (!g) return err(CELL_ESRCH);
    std::shared_ptr<EventQueue>* ep = et == SYS_SPU_THREAD_GROUP_EVENT_SYSTEM_MODULE ? &g->ep_sysmodule
        : et == SYS_SPU_THREAD_GROUP_EVENT_EXCEPTION                               ? &g->ep_exception
        : et == SYS_SPU_THREAD_GROUP_EVENT_RUN                                     ? &g->ep_run
                                                                                   : nullptr;
    if (!ep) {
        PS_LOG_ERROR(kSpuChan, "sys_spu_thread_group_connect_event({:#x}): unknown event type {}", id, et);
        return err(CELL_EINVAL);
    }
    if (et == SYS_SPU_THREAD_GROUP_EVENT_SYSTEM_MODULE && !(g->type & SYS_SPU_THREAD_GROUP_TYPE_COOPERATE_WITH_SYSTEM)) {
        return err(CELL_EINVAL);
    }
    auto queue = t.kernel.event_queue(eq);
    std::lock_guard lock(g->lock);
    if (alive(*ep)) return err(CELL_EBUSY);
    if (!queue) return err(CELL_ESRCH); // after EBUSY (RPCS3)
    *ep = std::move(queue);
    return CELL_OK;
}

Result sys_spu_thread_group_disconnect_event(PpuThread& t, u32 id, u32 et)
{
    const auto g = get_group(t.kernel.impl(), id);
    if (!g) return err(CELL_ESRCH);
    std::lock_guard lock(g->lock);
    std::shared_ptr<EventQueue>* ep = et == SYS_SPU_THREAD_GROUP_EVENT_SYSTEM_MODULE ? &g->ep_sysmodule
        : et == SYS_SPU_THREAD_GROUP_EVENT_EXCEPTION                               ? &g->ep_exception
        : et == SYS_SPU_THREAD_GROUP_EVENT_RUN                                     ? &g->ep_run
                                                                                   : nullptr;
    if (!ep) {
        PS_LOG_ERROR(kSpuChan, "sys_spu_thread_group_disconnect_event({:#x}): unknown event type {} (CELL_OK, as RPCS3)", id, et);
        return CELL_OK;
    }
    ep->reset(); // no checks (RPCS3)
    return CELL_OK;
}

Result sys_spu_thread_connect_event(PpuThread& t, u32 id, u32 eq, u32 et, u32 spup)
{
    const auto [g, th] = get_thread(t.kernel.impl(), id);
    auto queue = t.kernel.event_queue(eq);
    if (!queue || !th) return err(CELL_ESRCH);
    if (et != SYS_SPU_THREAD_EVENT_USER || spup > 63) {
        PS_LOG_ERROR(kSpuChan, "sys_spu_thread_connect_event({:#x}): event type {}, port {}", id, et, spup);
        return err(CELL_EINVAL);
    }
    std::lock_guard lock(g->lock);
    if (alive(th->ports[spup])) return err(CELL_EISCONN);
    th->ports[spup] = std::move(queue);
    return CELL_OK;
}

Result sys_spu_thread_disconnect_event(PpuThread& t, u32 id, u32 et, u32 spup)
{
    const auto [g, th] = get_thread(t.kernel.impl(), id);
    if (!th) return err(CELL_ESRCH);
    if (et != SYS_SPU_THREAD_EVENT_USER || spup > 63) {
        PS_LOG_ERROR(kSpuChan, "sys_spu_thread_disconnect_event({:#x}): event type {}, port {}", id, et, spup);
        return err(CELL_EINVAL);
    }
    std::lock_guard lock(g->lock);
    if (!alive(th->ports[spup])) return err(CELL_ENOTCONN);
    th->ports[spup].reset();
    return CELL_OK;
}

Result sys_spu_thread_bind_queue(PpuThread& t, u32 id, u32 spuq, u32 spuq_num)
{
    const auto [g, th] = get_thread(t.kernel.impl(), id);
    auto queue = t.kernel.event_queue(spuq);
    if (!queue || !th) return err(CELL_ESRCH);
    if (queue->type != SYS_SPU_QUEUE) return err(CELL_EINVAL);
    std::lock_guard lock(g->lock);
    std::pair<u32, std::shared_ptr<EventQueue>>* free_slot = nullptr;
    for (auto& v : th->spuq) {
        if (!v.second) {
            if (!free_slot) free_slot = &v;
            continue;
        }
        if (v.first == spuq_num || v.second == queue) return err(CELL_EBUSY);
    }
    if (!free_slot) return err(CELL_EAGAIN);
    *free_slot = {spuq_num, std::move(queue)};
    return CELL_OK;
}

Result sys_spu_thread_unbind_queue(PpuThread& t, u32 id, u32 spuq_num)
{
    const auto [g, th] = get_thread(t.kernel.impl(), id);
    if (!th) return err(CELL_ESRCH);
    std::lock_guard lock(g->lock);
    for (auto& v : th->spuq) {
        if (v.first == spuq_num && v.second) {
            v.second.reset();
            return CELL_OK;
        }
    }
    return err(CELL_ESRCH);
}

Result sys_spu_thread_group_connect_event_all_threads(PpuThread& t, u32 id, u32 eq, u64 req, u32 spup)
{
    Kernel::Impl& s = t.kernel.impl();
    if (!req) return err(CELL_EINVAL);
    const auto g = get_group(s, id);
    const auto queue = t.kernel.event_queue(eq);
    if (!g || !queue) return err(CELL_ESRCH);
    u8 port = 0;
    {
        std::lock_guard lock(g->lock);
        if (g->state < SpuGroupState::Initialized || g->state == SpuGroupState::Destroyed) {
            return err(g->state == SpuGroupState::Destroyed ? CELL_ESRCH : CELL_ESTAT);
        }
        // The lowest requested port free on every thread.
        for (; port < 64; port++) {
            if (!(req & (u64{1} << port))) continue;
            bool found = true;
            for (const auto& th : g->threads)
                if (th && alive(th->ports[port])) found = false;
            if (found) break;
        }
        if (port == 64) return err(CELL_EISCONN);
        for (const auto& th : g->threads)
            if (th) th->ports[port] = queue;
    }
    if (!s.guest.valid(spup, 1)) return err(CELL_EFAULT);
    s.guest.w8(spup, port);
    return CELL_OK;
}

Result sys_spu_thread_group_disconnect_event_all_threads(PpuThread& t, u32 id, u32 spup)
{
    if (spup > 63) return err(CELL_EINVAL);
    const auto g = get_group(t.kernel.impl(), id);
    if (!g) return err(CELL_ESRCH);
    std::lock_guard lock(g->lock);
    for (const auto& th : g->threads)
        if (th) th->ports[spup].reset();
    return CELL_OK;
}

// sys_spu_thread_group_set_cooperative_victims: checked as RPCS3, then nothing (RPCS3's TODO).
Result sys_spu_thread_group_set_cooperative_victims(PpuThread& t, u32 id, u32 threads_mask)
{
    const auto g = get_group(t.kernel.impl(), id);
    if (!g) return err(CELL_ESRCH);
    if (!(g->type & SYS_SPU_THREAD_GROUP_TYPE_COOPERATE_WITH_SYSTEM) || threads_mask >= (1u << g->max_num)) return err(CELL_EINVAL);
    return CELL_OK;
}

// sys_spu_thread_group_syscall_253 (cellSpursGetInfo's deadline counters): zeros and the time, as RPCS3.
Result sys_spu_thread_group_syscall_253(PpuThread& t, u32 id, u32 info)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto g = get_group(s, id);
    if (!g) return err(CELL_ESRCH);
    if (!(g->type & SYS_SPU_THREAD_GROUP_TYPE_COOPERATE_WITH_SYSTEM)) return err(CELL_EINVAL);
    if (!s.guest.valid(info, 16)) return err(CELL_EFAULT);
    s.guest.w32(info, 0);
    s.guest.w32(info + 4, 0);
    s.guest.w64(info + 8, ps_spu_timebase());
    return CELL_OK;
}

void register_spu_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 155, _sys_spu_image_get_information);
    PS_LV2_BIND(table, 156, sys_spu_image_open);
    PS_LV2_BIND(table, 157, _sys_spu_image_import);
    PS_LV2_BIND(table, 158, _sys_spu_image_close);
    PS_LV2_BIND(table, 159, _sys_spu_image_get_segments);
    PS_LV2_BIND(table, 165, sys_spu_thread_get_exit_status);
    PS_LV2_BIND(table, 166, sys_spu_thread_set_argument);
    PS_LV2_BIND(table, 169, sys_spu_initialize);
    PS_LV2_BIND(table, 170, sys_spu_thread_group_create);
    PS_LV2_BIND(table, 171, sys_spu_thread_group_destroy);
    PS_LV2_BIND(table, 172, sys_spu_thread_initialize);
    PS_LV2_BIND(table, 173, sys_spu_thread_group_start);
    PS_LV2_BIND(table, 174, sys_spu_thread_group_suspend);
    PS_LV2_BIND(table, 175, sys_spu_thread_group_resume);
    PS_LV2_BIND(table, 176, sys_spu_thread_group_yield);
    PS_LV2_BIND(table, 177, sys_spu_thread_group_terminate);
    PS_LV2_BIND(table, 178, sys_spu_thread_group_join);
    PS_LV2_BIND(table, 179, sys_spu_thread_group_set_priority);
    PS_LV2_BIND(table, 180, sys_spu_thread_group_get_priority);
    PS_LV2_BIND(table, 181, sys_spu_thread_write_ls);
    PS_LV2_BIND(table, 182, sys_spu_thread_read_ls);
    PS_LV2_BIND(table, 184, sys_spu_thread_write_snr);
    PS_LV2_BIND(table, 185, sys_spu_thread_group_connect_event);
    PS_LV2_BIND(table, 186, sys_spu_thread_group_disconnect_event);
    PS_LV2_BIND(table, 187, sys_spu_thread_set_spu_cfg);
    PS_LV2_BIND(table, 188, sys_spu_thread_get_spu_cfg);
    PS_LV2_BIND(table, 190, sys_spu_thread_write_spu_mb);
    PS_LV2_BIND(table, 191, sys_spu_thread_connect_event);
    PS_LV2_BIND(table, 192, sys_spu_thread_disconnect_event);
    PS_LV2_BIND(table, 193, sys_spu_thread_bind_queue);
    PS_LV2_BIND(table, 194, sys_spu_thread_unbind_queue);
    PS_LV2_BIND(table, 198, sys_spu_thread_recover_page_fault);
    PS_LV2_BIND(table, 250, sys_spu_thread_group_set_cooperative_victims);
    PS_LV2_BIND(table, 251, sys_spu_thread_group_connect_event_all_threads);
    PS_LV2_BIND(table, 252, sys_spu_thread_group_disconnect_event_all_threads);
    PS_LV2_BIND(table, 253, sys_spu_thread_group_syscall_253);
    PS_LV2_BIND(table, 254, sys_spu_thread_group_log);
    PS_LV2_BIND(table, 260, sys_spu_image_open_by_fd);
}

} // namespace pspocket::lv2
