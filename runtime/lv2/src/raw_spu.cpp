// Raw and isolated SPUs, interrupt tags and interrupt threads (D-528). References: RPCS3 rpcs3/Emu/Cell/lv2/sys_spu.cpp
// (sys_raw_spu_*, sys_isolated_spu_*), Emu/Cell/RawSPUThread.cpp (the problem-state registers), lv2/sys_interrupt.cpp
// (interrupt tags, services and threads), Emu/Cell/SPUThread.h (register offsets, interrupt bits).
//
// A raw SPU (index 0..4, within sys_spu_initialize's share) has its local store in guest memory at 0xE0000000 +
// 0x100000 * index, which the PPU reads and writes directly, and its problem-state registers 0x40000 above it: a guest
// MMIO range (pspocket/core/mmio.hpp) whose loads and stores fault and are performed on the registers here. The SPU
// runs on pspocket's SPU runtime on its own host thread: SPU_RunCntl 1 starts it at SPU_NPC (bit 0 enables its
// interrupts), 0 stops it (the write waits until it has); every stop code stops it (status: stopped by stop, the code
// in the upper half), as does a halt; its outbound interrupt mailbox is the plain one-entry mailbox. The MFC proxy
// (LSA, EAH, EAL, size and tag, command) runs a GET, PUT or SDCRZ when the command status is read after all five were
// written (else MFC_PPU_DMA_CMD_SEQUENCE_ERROR), starting the SPU for the S forms; barriers and syncs are fences.
//
// Interrupts: classes 0 and 2 each have a mask, a status and a tag. Class 2's status gets the mailbox bit when the SPU
// writes its interrupt mailbox, the stop-and-signal bit when it stops, the halt bit when it halts. A tag's handler is
// an interrupt thread: a PPU thread created with SYS_PPU_THREAD_CREATE_INTERRUPT and established on the tag. Whenever
// a class-2 status bit its mask lets through is set (the mailbox bit too while the interrupt mailbox holds a value, as
// RPCS3's loop sets it), the thread runs its entry with (arg1, arg2) from the top of its stack until
// sys_interrupt_thread_eoi (or the entry's return), then waits again; disestablishing ends it (its r13 given back).
//
// Isolated SPUs: a raw SPU loaded from an isolated SPU module (a SELF in guest memory, through the process's SELF
// decrypter), r3..r6 its arguments, its status marked isolated, started by sys_isolated_spu_start. RPCS3's table marks
// these root's, but RPCS3 lets a game use them, and so does pspocket.
//
// Without fault emulation (mmio_supported(): Apple AArch64 only) a raw SPU's registers cannot answer the PPU:
// sys_raw_spu_create logs that and answers CELL_EAGAIN, as when no raw SPU is left.

#include <pthread.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <format>
#include <mutex>
#include <span>
#include <vector>

#include "pspocket/core/mmio.hpp"
#include "pspocket/spu/fp.h"
#include "pspocket/spu/runtime.h"
#include "spu.hpp"

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace pspocket::lv2 {

// Thrown by sys_interrupt_thread_eoi on an interrupt thread: back to its wait.
struct InterruptReturn {};

namespace {

constexpr std::string_view kRawChan = "raw_spu";
constexpr u32 kRawBase = 0xE0000000, kRawOffset = 0x100000, kProbOffset = 0x40000, kProbSize = 0x20000;
constexpr u32 kLsSize = 0x40000;
constexpr u32 kMaxRaw = 5;

// Problem-state register offsets (RPCS3 SPUThread.h).
enum : u32 {
    MFC_LSA_offs = 0x3004,
    MFC_EAH_offs = 0x3008,
    MFC_EAL_offs = 0x300C,
    MFC_Size_Tag_offs = 0x3010,
    MFC_Class_CMD_offs = 0x3014,
    MFC_CMDStatus_offs = 0x3014,
    MFC_QStatus_offs = 0x3104,
    Prxy_QueryType_offs = 0x3204,
    Prxy_QueryMask_offs = 0x321C,
    Prxy_TagStatus_offs = 0x322C,
    SPU_Out_MBox_offs = 0x4004,
    SPU_In_MBox_offs = 0x400C,
    SPU_MBox_Status_offs = 0x4014,
    SPU_RunCntl_offs = 0x401C,
    SPU_Status_offs = 0x4024,
    SPU_NPC_offs = 0x4034,
    SPU_RdSigNotify1_offs = 0x1400C,
    SPU_RdSigNotify2_offs = 0x1C00C,
};
enum : u64 {
    SPU_INT2_STAT_MAILBOX_INT = 1ull << 0,
    SPU_INT2_STAT_SPU_STOP_AND_SIGNAL_INT = 1ull << 1,
    SPU_INT2_STAT_SPU_HALT_OR_STEP_INT = 1ull << 2,
    SPU_INT2_STAT_DMA_TAG_GROUP_COMPLETION_INT = 1ull << 3,
    SPU_INT0_STAT_SPU_ERROR_INT = 1ull << 2,
};
enum : u32 {
    SPU_RUNCNTL_STOP_REQUEST = 0,
    SPU_RUNCNTL_RUN_REQUEST = 1,
    SPU_STATUS_IS_ISOLATED = 0x80,
    MFC_PPU_DMA_CMD_ENQUEUE_SUCCESSFUL = 0,
    MFC_PPU_DMA_CMD_SEQUENCE_ERROR = 1,
    MFC_PROXY_COMMAND_QUEUE_EMPTY_FLAG = 0x80000000,
    MFC_START_MASK = 0x08,
};

class InterruptService;

class InterruptTag final : public Object {
public:
    static constexpr u32 kIdBase = 0x0A000000; // RPCS3 lv2_int_tag
    u32 id_base() const override { return kIdBase; }
    u32 raw = 0, cls = 0;                      // its raw SPU's index and the interrupt class
    std::shared_ptr<InterruptService> handler; // under RawSpus::mutex
};

class InterruptService final : public Object {
public:
    static constexpr u32 kIdBase = 0x0B000000; // RPCS3 lv2_int_serv
    u32 id_base() const override { return kIdBase; }
    std::shared_ptr<PpuThread> thread;
    u64 arg1 = 0, arg2 = 0;
    std::shared_ptr<InterruptTag> tag;
    std::mutex m;
    std::condition_variable cv;
    u64 kicks = 0;     // under m
    bool quit = false; // under m
    bool done = false; // under m: the host thread has left guest code for good
};

struct IntCtrl {
    std::atomic<u64> mask{0}, stat{0};
    std::shared_ptr<InterruptTag> tag; // under RawSpus::mutex
};

class RawSpu final : public MmioDevice {
public:
    RawSpu(Kernel::Impl& s, u32 index, bool isolated);
    ~RawSpu() override;

    u64 read(u32 offset, u32 size) override;
    void write(u32 offset, u32 size, u64 value) override;

    u32 read_reg(u32 offset);
    void write_reg(u32 offset, u32 value);
    bool start_host();
    void quit_and_join();
    void start();
    void stop_and_wait();
    void raise(u32 cls, u64 bits);
    // RPCS3 try_start marks the SPU running at once: so does a run request here, until its run ends.
    u32 status()
    {
        const u32 st = ps_spu_status(&ctx);
        const bool busy = busy_.load(std::memory_order_acquire);
        return (busy && !(st & PS_SPU_STATUS_RUNNING) ? u32{PS_SPU_STATUS_RUNNING} : st) |
               (isolated ? SPU_STATUS_IS_ISOLATED : 0);
    }

    Kernel::Impl& s;
    const u32 index;
    const bool isolated;
    const u32 ls_addr;
    SPUContext ctx{};
    std::array<IntCtrl, 3> int_ctrl;
    std::atomic<u32> run_ctrl{0};
    std::atomic<u32> npc{0};
    std::atomic<u32> snr_config{0};

private:
    static void* entry(void* arg);
    void body();

    std::mutex mfc_m_;
    u32 lsa_ = 0, eah_ = 0, eal_ = 0, size_ = 0, tag_ = 0, cmd_ = 0, written_ = 0;
    std::atomic<u32> prxy_mask_{0};

    std::mutex run_m_;
    std::condition_variable run_cv_;
    u64 starts_ = 0, runs_ = 0;              // under run_m_
    bool quit_ = false, stop_pending_ = false; // under run_m_
    std::atomic<bool> busy_{false};          // a run requested and not yet over (set under run_m_)
    pthread_t host_{};
    bool joinable_ = false;
};

struct RawSpus {
    std::mutex mutex;
    std::array<std::shared_ptr<RawSpu>, kMaxRaw> raw{};
};

RawSpus& raws(Kernel::Impl& s)
{
    std::lock_guard lock(s.system_mutex);
    if (!s.raw_spu_state) s.raw_spu_state = std::make_shared<RawSpus>();
    return *static_cast<RawSpus*>(s.raw_spu_state.get());
}

void kick(const std::shared_ptr<InterruptService>& sv)
{
    if (!sv) return;
    {
        std::lock_guard lock(sv->m);
        sv->kicks++;
    }
    sv->cv.notify_all();
}

// Raw SPU hooks: every stop code stops the SPU; the interrupt mailbox is the plain one, its writes raise class 2.
int raw_stop(SPUContext*, uint32_t) { return 0; }
void raw_intr_mbox_written(SPUContext* ctx)
{
    static_cast<RawSpu*>(ctx->runtime)->raise(2, SPU_INT2_STAT_MAILBOX_INT);
}
constexpr ps_spu_hooks kRawHooks{raw_stop, nullptr, 1, raw_intr_mbox_written};

RawSpu::RawSpu(Kernel::Impl& s, u32 index, bool isolated)
    : s(s), index(index), isolated(isolated), ls_addr(kRawBase + kRawOffset * index)
{
    ps_spu_context_init(&ctx, s.guest.ptr(ls_addr), s.kernel.memory().base());
    ctx.id = index;
    ctx.index = index;
    ctx.runtime = this;
    ctx.hooks = &kRawHooks;
    ps_spu_host_attach(&ctx);
}

RawSpu::~RawSpu()
{
    quit_and_join();
    mmio_unregister(ls_addr + kProbOffset);
    ps_spu_host_detach(&ctx);
}

bool RawSpu::start_host()
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
#if defined(__APPLE__)
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INITIATED, 0);
#endif
    const int rc = pthread_create(&host_, &attr, &RawSpu::entry, this);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        PS_LOG_ERROR(kRawChan, "raw SPU {}: no host thread (error {})", index, rc);
        return false;
    }
    joinable_ = true;
    return true;
}

void RawSpu::quit_and_join()
{
    if (!joinable_) return;
    {
        std::lock_guard lock(run_m_);
        quit_ = true;
    }
    run_cv_.notify_all();
    ps_spu_request_stop(&ctx);
    pthread_join(host_, nullptr);
    joinable_ = false;
}

void* RawSpu::entry(void* arg)
{
    static_cast<RawSpu*>(arg)->body();
    return nullptr;
}

void RawSpu::body()
{
#if defined(__APPLE__)
    pthread_setname_np(std::format("Raw SPU {}", index).c_str());
#endif
    const u64 fp_env = ps_spu_fpenv_enter();
    for (;;) {
        {
            std::unique_lock lock(run_m_);
            run_cv_.wait(lock, [&] { return quit_ || starts_ != runs_; });
            if (quit_) break;
            runs_ = starts_;
            if (stop_pending_) { // stopped before it began
                stop_pending_ = false;
                busy_.store(false, std::memory_order_release);
                run_cv_.notify_all();
                continue;
            }
            const u32 n = npc.load(std::memory_order_acquire);
            ctx.pc = n & 0x3FFFC;
            ctx.interrupts_enabled = n & 1;
            ps_spu_clear_stop(&ctx); // under run_m_, where a stop request is made: none is lost
        }
        const u32 reason = ps_spu_run(&ctx);
        npc.store((ctx.pc & 0x3FFFC) | (ctx.interrupts_enabled ? 1u : 0u), std::memory_order_release);
        {
            std::lock_guard lock(run_m_);
            stop_pending_ = false;
            busy_.store(false, std::memory_order_release);
        }
        run_cv_.notify_all();
        switch (reason) {
        case PS_SPU_STOP_SIGNAL: raise(2, SPU_INT2_STAT_SPU_STOP_AND_SIGNAL_INT); break;
        case PS_SPU_STOP_HALT: raise(2, SPU_INT2_STAT_SPU_HALT_OR_STEP_INT); break;
        case PS_SPU_STOP_EXCEPTION:
            PS_LOG_ERROR(kRawChan, "raw SPU {}: exception {} at {:#07x} (detail {:#x}); it stops", index, ctx.exception,
                ctx.pc, ctx.stop_code);
            raise(0, SPU_INT0_STAT_SPU_ERROR_INT);
            break;
        default: break; // a stop request
        }
    }
    ps_spu_fpenv_leave(fp_env);
}

// SPU_RunCntl 1: the SPU starts unless it runs (RPCS3 try_start).
void RawSpu::start()
{
    {
        std::lock_guard lock(run_m_);
        if (busy_.load(std::memory_order_relaxed)) return;
        busy_.store(true, std::memory_order_release);
        starts_++;
    }
    run_cv_.notify_all();
}

// SPU_RunCntl 0: the SPU stops; the write returns once it has.
void RawSpu::stop_and_wait()
{
    std::unique_lock lock(run_m_);
    if (!busy_.load(std::memory_order_relaxed)) return;
    stop_pending_ = true;
    ps_spu_request_stop(&ctx);
    run_cv_.wait(lock, [&] { return quit_ || !busy_.load(std::memory_order_relaxed); });
}

void RawSpu::raise(u32 cls, u64 bits)
{
    IntCtrl& c = int_ctrl[cls];
    c.stat.fetch_or(bits, std::memory_order_acq_rel);
    if (!(c.mask.load(std::memory_order_acquire) & bits)) return;
    std::shared_ptr<InterruptService> handler;
    {
        RawSpus& r = raws(s);
        std::lock_guard lock(r.mutex);
        if (c.tag) handler = c.tag->handler;
    }
    kick(handler);
}

u64 RawSpu::read(u32 offset, u32 size)
{
    if (size == 8) return (u64{read_reg(offset)} << 32) | read_reg(offset + 4);
    const u32 word = read_reg(offset & ~3u);
    if (size == 4) return word;
    const u32 shift = (4 - size - (offset & 3)) * 8;
    return (word >> shift) & ((1u << (size * 8)) - 1);
}

void RawSpu::write(u32 offset, u32 size, u64 value)
{
    if (size == 8) {
        write_reg(offset, static_cast<u32>(value >> 32));
        write_reg(offset + 4, static_cast<u32>(value));
        return;
    }
    if (size != 4) {
        PS_LOG_ERROR(kRawChan, "raw SPU {}: a {}-byte write to register {:#x} (only 4 and 8 bytes are taken)", index, size,
            offset);
        return;
    }
    write_reg(offset, static_cast<u32>(value));
}

u32 RawSpu::read_reg(u32 offset)
{
    switch (offset) {
    case MFC_CMDStatus_offs: {
        u32 lsa, ea, size, cmd;
        {
            std::lock_guard lock(mfc_m_);
            if (written_ != 0x1F) return MFC_PPU_DMA_CMD_SEQUENCE_ERROR;
            written_ = 0;
            lsa = lsa_, ea = eal_, size = size_, cmd = cmd_;
            if (eah_) PS_LOG_ERROR(kRawChan, "raw SPU {}: MFC proxy EAH {:#x} (only 32-bit addresses)", index, eah_);
        }
        const u32 c = cmd & 0xFF;
        if (c == 0xC0 || c == 0xC8 || c == 0xCC) { // barrier, eieio, sync
            std::atomic_thread_fence(std::memory_order_seq_cst);
            return MFC_PPU_DMA_CMD_ENQUEUE_SUCCESSFUL;
        }
        if (c == 0x8B || c == 0x8D) return MFC_PPU_DMA_CMD_ENQUEUE_SUCCESSFUL; // cache-line hints (SDCRT, SDCRTST)
        if (size && !s.guest.valid(ea, size)) {
            // The MFC takes the command and faults on the address afterwards (a page fault lv2 would end the process
            // for); here nothing is copied. Logged: a copy from there would fault inside this fault handler.
            PS_LOG_ERROR(kRawChan, "raw SPU {}: MFC proxy command {:#x} with EA {:#x} ({:#x} bytes) outside guest memory",
                index, c, ea, size);
            return MFC_PPU_DMA_CMD_ENQUEUE_SUCCESSFUL;
        }
        if (ps_spu_proxy_dma(&ctx, c, lsa, ea, size) != 0) {
            PS_LOG_ERROR(kRawChan, "raw SPU {}: MFC proxy command {:#x} (LSA {:#x}, EA {:#x}, size {:#x}) not taken",
                index, c, lsa, ea, size);
            return MFC_PPU_DMA_CMD_SEQUENCE_ERROR;
        }
        if (c & MFC_START_MASK) start();
        return MFC_PPU_DMA_CMD_ENQUEUE_SUCCESSFUL;
    }
    case MFC_QStatus_offs: return MFC_PROXY_COMMAND_QUEUE_EMPTY_FLAG | 8;
    case Prxy_QueryMask_offs:
    case Prxy_TagStatus_offs: return prxy_mask_.load(std::memory_order_acquire);
    case Prxy_QueryType_offs: return 0;
    case SPU_Out_MBox_offs: {
        u32 v = 0;
        ps_spu_read_out_mbox(&ctx, &v);
        return v;
    }
    case SPU_MBox_Status_offs: {
        const u32 out = __atomic_load_n(&ctx.ch.out_mbox_count, __ATOMIC_ACQUIRE);
        const u32 in = 4 - ps_spu_in_mbox_count(&ctx);
        const u32 intr = __atomic_load_n(&ctx.ch.out_intr_mbox_count, __ATOMIC_ACQUIRE);
        return (out & 0xFF) | ((in << 8) & 0xFF00) | ((intr << 16) & 0xFF0000);
    }
    case SPU_Status_offs: return status();
    case SPU_NPC_offs: return (status() & PS_SPU_STATUS_RUNNING) ? 0 : npc.load(std::memory_order_acquire);
    case SPU_RunCntl_offs: return run_ctrl.load(std::memory_order_acquire);
    default:
        PS_LOG_ERROR(kRawChan, "raw SPU {}: read of an unknown problem-state register {:#x}", index, offset);
        return 0;
    }
}

void RawSpu::write_reg(u32 offset, u32 value)
{
    switch (offset) {
    case MFC_LSA_offs: {
        std::lock_guard lock(mfc_m_);
        lsa_ = value & (kLsSize - 1);
        written_ |= 1;
        return;
    }
    case MFC_EAH_offs: {
        std::lock_guard lock(mfc_m_);
        eah_ = value;
        written_ |= 2;
        return;
    }
    case MFC_EAL_offs: {
        std::lock_guard lock(mfc_m_);
        eal_ = value;
        written_ |= 4;
        return;
    }
    case MFC_Size_Tag_offs: {
        std::lock_guard lock(mfc_m_);
        tag_ = value & 0x1F;
        size_ = (value >> 16) & 0x7FFF;
        written_ |= 8;
        return;
    }
    case MFC_Class_CMD_offs: {
        std::lock_guard lock(mfc_m_);
        cmd_ = value & 0xFF;
        written_ |= 0x10;
        return;
    }
    case Prxy_QueryType_offs:
        if (value > 2) break;
        if (value) raise(2, SPU_INT2_STAT_DMA_TAG_GROUP_COMPLETION_INT); // the proxy's commands are done at once
        return;
    case Prxy_QueryMask_offs: prxy_mask_.store(value, std::memory_order_release); return;
    case SPU_In_MBox_offs: ps_spu_write_in_mbox(&ctx, value); return;
    case SPU_RunCntl_offs:
        if (value != SPU_RUNCNTL_RUN_REQUEST && value != SPU_RUNCNTL_STOP_REQUEST) break;
        run_ctrl.store(value, std::memory_order_release);
        if (value == SPU_RUNCNTL_RUN_REQUEST) start();
        else stop_and_wait();
        return;
    case SPU_NPC_offs:
        if (!(status() & PS_SPU_STATUS_RUNNING)) npc.store(value & 0x3FFFD, std::memory_order_release);
        return;
    case SPU_RdSigNotify1_offs: ps_spu_write_signal(&ctx, 1, value); return;
    case SPU_RdSigNotify2_offs: ps_spu_write_signal(&ctx, 2, value); return;
    default: break;
    }
    PS_LOG_ERROR(kRawChan, "raw SPU {}: write of {:#x} to an unknown problem-state register {:#x}", index, value, offset);
}

std::shared_ptr<RawSpu> find_raw(Kernel::Impl& s, u32 id, bool isolated)
{
    if (id >= kMaxRaw) return nullptr;
    RawSpus& r = raws(s);
    std::lock_guard lock(r.mutex);
    const auto& raw = r.raw[id];
    return raw && raw->isolated == isolated ? raw : nullptr;
}

// ------------------------------------------------------------------------------------------------- interrupt threads

// Whether a raw SPU's class-2 interrupt this service handles is pending (RPCS3 ppu_interrupt_thread_entry's loop).
bool pending(Kernel::Impl& s, const InterruptService& sv)
{
    RawSpus& r = raws(s);
    std::lock_guard lock(r.mutex);
    for (const auto& raw : r.raw) {
        if (!raw) continue;
        IntCtrl& c = raw->int_ctrl[2];
        if (!c.tag || c.tag->handler.get() != &sv) continue;
        const u64 mask = c.mask.load(std::memory_order_acquire);
        if ((mask & SPU_INT2_STAT_MAILBOX_INT) && __atomic_load_n(&raw->ctx.ch.out_intr_mbox_count, __ATOMIC_ACQUIRE)) {
            c.stat.fetch_or(SPU_INT2_STAT_MAILBOX_INT, std::memory_order_acq_rel);
        }
        if (mask & c.stat.load(std::memory_order_acquire)) return true;
    }
    return false;
}

struct ServiceArgs {
    Kernel* kernel;
    std::shared_ptr<InterruptService> sv;
};

void* service_main(void* raw)
{
    std::unique_ptr<ServiceArgs> args(static_cast<ServiceArgs*>(raw));
    Kernel& k = *args->kernel;
    Kernel::Impl& s = k.impl();
    InterruptService& sv = *args->sv;
    PpuThread& t = *sv.thread;
    set_current_thread(&t);
#if defined(__APPLE__)
    pthread_setname_np(std::format("PPU {:#x} interrupt", t.id).c_str());
#endif
    for (;;) {
        {
            std::unique_lock lock(sv.m);
            u64 seen = sv.kicks;
            for (;;) {
                if (sv.quit || s.exiting.load(std::memory_order_relaxed)) break;
                lock.unlock();
                const bool fire = pending(s, sv);
                lock.lock();
                if (fire) break;
                sv.cv.wait(lock, [&] { return sv.quit || sv.kicks != seen || s.exiting.load(std::memory_order_relaxed); });
                seen = sv.kicks;
            }
            if (sv.quit || s.exiting.load(std::memory_order_relaxed)) break;
        }
        PPUContext& c = *t.ctx;
        c.gpr[1] = t.stack_addr + t.stack_size - kStackStartOffset;
        c.gpr[2] = t.rtoc;
        c.gpr[3] = sv.arg1;
        c.gpr[4] = sv.arg2;
        c.gpr[13] = t.tls;
        c.lr = kThreadReturnAddress;
        c.cia = t.entry;
        try {
            run_guest(k, c, k.base(), t.entry, true);
        } catch (const InterruptReturn&) {
        } catch (const ThreadExit&) {
            break;
        }
    }
    thread_finish(t, 0);
    set_current_thread(nullptr);
    {
        std::lock_guard lock(sv.m);
        sv.done = true;
    }
    sv.cv.notify_all();
    const std::shared_ptr<HostThreads> host = s.host_threads;
    args.reset();
    std::lock_guard lock(host->mutex);
    host->live--;
    host->cv.notify_all();
    return nullptr;
}

// Ends the service: its thread leaves its wait (or its handler, at the next eoi) and is joined.
void end_service(Kernel::Impl& s, const std::shared_ptr<InterruptService>& sv)
{
    {
        std::unique_lock lock(sv->m);
        sv->quit = true;
        sv->cv.notify_all();
        if (current_thread() != sv->thread.get()) sv->cv.wait(lock, [&] { return sv->done; });
    }
    if (sv->tag) {
        RawSpus& r = raws(s);
        std::lock_guard lock(r.mutex);
        if (sv->tag->handler == sv) sv->tag->handler = nullptr;
    }
    s.objects.remove(InterruptService::kIdBase, sv->id);
}

Result create_tag(PpuThread& t, u32 id, u32 cls, u32 intrtag, bool isolated)
{
    Kernel::Impl& s = t.kernel.impl();
    if (cls != 0 && cls != 2) return err(CELL_EINVAL);
    const auto raw = find_raw(s, id, isolated);
    if (!raw) return err(CELL_ESRCH);
    if (!s.guest.valid(intrtag, 4)) return err(CELL_EFAULT);
    RawSpus& r = raws(s);
    std::lock_guard lock(r.mutex);
    IntCtrl& c = raw->int_ctrl[cls];
    if (c.tag) return err(CELL_EAGAIN);
    auto tag = std::make_shared<InterruptTag>();
    tag->raw = id;
    tag->cls = cls;
    const u32 tid = s.objects.insert(InterruptTag::kIdBase, tag);
    if (!tid) return err(CELL_EAGAIN);
    tag->id = tid;
    c.tag = tag;
    s.guest.w32(intrtag, tid);
    return CELL_OK;
}

Result set_mask(PpuThread& t, u32 id, u32 cls, u64 mask, bool isolated)
{
    if (cls != 0 && cls != 2) return err(CELL_EINVAL);
    const auto raw = find_raw(t.kernel.impl(), id, isolated);
    if (!raw) return err(CELL_ESRCH);
    raw->int_ctrl[cls].mask.store(mask, std::memory_order_release);
    std::shared_ptr<InterruptService> handler;
    {
        RawSpus& r = raws(t.kernel.impl());
        std::lock_guard lock(r.mutex);
        if (raw->int_ctrl[cls].tag) handler = raw->int_ctrl[cls].tag->handler;
    }
    kick(handler); // a newly let-through mailbox interrupt
    return CELL_OK;
}

Result set_stat(PpuThread& t, u32 id, u32 cls, u64 stat, bool isolated)
{
    if (cls != 0 && cls != 2) return err(CELL_EINVAL);
    const auto raw = find_raw(t.kernel.impl(), id, isolated);
    if (!raw) return err(CELL_ESRCH);
    raw->int_ctrl[cls].stat.fetch_and(~stat, std::memory_order_acq_rel);
    return CELL_OK;
}

Result get_ctrl(PpuThread& t, u32 id, u32 cls, u32 out, bool isolated, bool mask)
{
    if (cls != 0 && cls != 2) return err(CELL_EINVAL);
    const auto raw = find_raw(t.kernel.impl(), id, isolated);
    if (!raw) return err(CELL_ESRCH);
    const IntCtrl& c = raw->int_ctrl[cls];
    if (!t.kernel.impl().guest.put64(out, (mask ? c.mask : c.stat).load(std::memory_order_acquire))) return err(CELL_EFAULT);
    return CELL_OK;
}

Result read_puint_mb(PpuThread& t, u32 id, u32 out, bool isolated)
{
    const auto raw = find_raw(t.kernel.impl(), id, isolated);
    if (!raw) return err(CELL_ESRCH);
    u32 v = 0;
    ps_spu_read_out_intr_mbox(&raw->ctx, &v);
    if (!t.kernel.impl().guest.put32(out, v)) return err(CELL_EFAULT);
    return CELL_OK;
}

Result set_cfg(PpuThread& t, u32 id, u32 value, bool isolated)
{
    if (value > 3) {
        PS_LOG_ERROR(kRawChan, "sys_raw_spu_set_spu_cfg({}, {:#x}): an unexpected value (RPCS3 aborts)", id, value);
        return err(CELL_EINVAL);
    }
    const auto raw = find_raw(t.kernel.impl(), id, isolated);
    if (!raw) return err(CELL_ESRCH);
    raw->snr_config.store(value, std::memory_order_release);
    ps_spu_set_signal_or_mode(&raw->ctx, 1, value & 1);
    ps_spu_set_signal_or_mode(&raw->ctx, 2, (value >> 1) & 1);
    return CELL_OK;
}

Result get_cfg(PpuThread& t, u32 id, u32 out, bool isolated)
{
    const auto raw = find_raw(t.kernel.impl(), id, isolated);
    if (!raw) return err(CELL_ESRCH);
    if (!t.kernel.impl().guest.put32(out, raw->snr_config.load(std::memory_order_acquire))) return err(CELL_EFAULT);
    return CELL_OK;
}

// A raw (or isolated) SPU: an index within the share, its local store committed, its registers mapped, its host thread.
Result make_raw(PpuThread& t, bool isolated, std::shared_ptr<RawSpu>& out)
{
    Kernel::Impl& s = t.kernel.impl();
    if (!mmio_supported()) {
        PS_LOG_ERROR(kRawChan, "raw SPUs need fault emulation for their registers, which this platform lacks");
        return err(CELL_EAGAIN);
    }
    std::lock_guard limits(s.spu_limits.mutex);
    RawSpus& r = raws(s);
    std::lock_guard lock(r.mutex);
    u32 count = 0, index = kMaxRaw;
    for (u32 i = 0; i < kMaxRaw; i++) {
        if (r.raw[i]) count++;
        else if (index == kMaxRaw) index = i;
    }
    if (count + 1 > s.spu_limits.max_raw || index == kMaxRaw) return err(CELL_EAGAIN);
    const u32 ls = kRawBase + kRawOffset * index;
    if (!t.kernel.memory().commit(ls, kLsSize)) return err(CELL_ENOMEM);
    std::memset(s.guest.ptr(ls), 0, kLsSize);
    auto raw = std::make_shared<RawSpu>(s, index, isolated);
    if (!mmio_register(t.kernel.base(), ls + kProbOffset, kProbSize, raw.get()) || !raw->start_host()) {
        mmio_unregister(ls + kProbOffset);
        t.kernel.memory().decommit(ls, kLsSize);
        return err(CELL_EAGAIN);
    }
    r.raw[index] = raw;
    out = raw;
    return CELL_OK;
}

Result destroy_raw(PpuThread& t, u32 id, bool isolated)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto raw = find_raw(s, id, isolated);
    if (!raw) return err(CELL_ESRCH);
    std::vector<std::shared_ptr<InterruptService>> services;
    std::vector<u32> tags;
    {
        RawSpus& r = raws(s);
        std::lock_guard lock(r.mutex);
        for (IntCtrl& c : raw->int_ctrl) {
            if (!c.tag) continue;
            if (c.tag->handler) services.push_back(c.tag->handler);
            tags.push_back(c.tag->id);
            c.tag = nullptr;
        }
        r.raw[id] = nullptr;
    }
    for (const auto& sv : services) end_service(s, sv);
    for (const u32 tag : tags) s.objects.remove(InterruptTag::kIdBase, tag);
    raw->quit_and_join();
    mmio_unregister(raw->ls_addr + kProbOffset);
    t.kernel.memory().decommit(raw->ls_addr, kLsSize);
    return CELL_OK;
}

} // namespace

// ------------------------------------------------------------------------------------------------- raw SPU calls

// sys_raw_spu_create(u32* id, attr).
Result sys_raw_spu_create(PpuThread& t, u32 id, u32)
{
    if (!t.kernel.impl().guest.valid(id, 4)) return err(CELL_EFAULT);
    std::shared_ptr<RawSpu> raw;
    if (const Result r = make_raw(t, false, raw); r != CELL_OK) return r;
    t.kernel.impl().guest.w32(id, raw->index);
    PS_LOG_INFO(kRawChan, "raw SPU {} created: local store at {:#010x}", raw->index, raw->ls_addr);
    return CELL_OK;
}

Result sys_raw_spu_destroy(PpuThread& t, u32 id) { return destroy_raw(t, id, false); }
Result sys_raw_spu_create_interrupt_tag(PpuThread& t, u32 id, u32 cls, u32, u32 intrtag)
{
    return create_tag(t, id, cls, intrtag, false);
}
Result sys_raw_spu_set_int_mask(PpuThread& t, u32 id, u32 cls, u64 mask) { return set_mask(t, id, cls, mask, false); }
Result sys_raw_spu_get_int_mask(PpuThread& t, u32 id, u32 cls, u32 out) { return get_ctrl(t, id, cls, out, false, true); }
Result sys_raw_spu_set_int_stat(PpuThread& t, u32 id, u32 cls, u64 stat) { return set_stat(t, id, cls, stat, false); }
Result sys_raw_spu_get_int_stat(PpuThread& t, u32 id, u32 cls, u32 out) { return get_ctrl(t, id, cls, out, false, false); }
Result sys_raw_spu_read_puint_mb(PpuThread& t, u32 id, u32 out) { return read_puint_mb(t, id, out, false); }
Result sys_raw_spu_set_spu_cfg(PpuThread& t, u32 id, u32 value) { return set_cfg(t, id, value, false); }
Result sys_raw_spu_get_spu_cfg(PpuThread& t, u32 id, u32 out) { return get_cfg(t, id, out, false); }

// sys_raw_spu_recover_page_fault(id): pspocket raises no page faults (D-184), so none waits to be recovered.
Result sys_raw_spu_recover_page_fault(PpuThread& t, u32 id)
{
    return find_raw(t.kernel.impl(), id, false) ? err(CELL_EINVAL) : err(CELL_ESRCH);
}

// ------------------------------------------------------------------------------------------------- isolated SPUs

// sys_isolated_spu_create(u32* id, image, arg1..arg4): the isolated SPU module (a SELF) at `image` in guest memory.
Result sys_isolated_spu_create(PpuThread& t, u32 id, u32 image, u64 arg1, u64 arg2, u64 arg3, u64 arg4)
{
    Kernel::Impl& s = t.kernel.impl();
    if (!s.guest.valid(id, 4)) return err(CELL_EFAULT);
    if (!s.guest.valid(image, 4)) return err(CELL_EFAULT);
    // The image runs to the end of its committed memory (RPCS3: "more accurate size calculation" a todo).
    u32 end = image & ~0xFFFu;
    while (end != 0u - 0x1000 && s.guest.valid(end, 0x1000)) end += 0x1000;
    std::vector<u8> bytes(s.guest.ptr(image), s.guest.ptr(image) + (end - image));
    if (decrypt_spu_self(s, bytes, "sys_isolated_spu_create") != CELL_OK) return err(CELL_EAUTHFAIL);
    if (!spu_elf_entry(bytes)) return err(CELL_EAUTHFAIL);
    std::shared_ptr<RawSpu> raw;
    if (const Result r = make_raw(t, true, raw); r != CELL_OK) return r;
    u32 entry = 0;
    load_spu_elf(bytes, s.guest.ptr(raw->ls_addr), entry);
    for (u32 i = 0; i < 4; i++) {
        const u64 a = i == 0 ? arg1 : i == 1 ? arg2 : i == 2 ? arg3 : arg4;
        std::memset(&raw->ctx.gpr[3 + i], 0, sizeof(raw->ctx.gpr[3 + i]));
        raw->ctx.gpr[3 + i].u64[1] = a; // the preferred doubleword (RPCS3 v128::from64(0, arg))
    }
    raw->npc.store(entry & 0x3FFFC, std::memory_order_release);
    s.guest.w32(id, raw->index);
    PS_LOG_INFO(kRawChan, "isolated SPU {} created: entry {:#x}", raw->index, entry);
    return CELL_OK;
}

Result sys_isolated_spu_destroy(PpuThread& t, u32 id) { return destroy_raw(t, id, true); }

// sys_isolated_spu_start(id): SPU_RunCntl's run request.
Result sys_isolated_spu_start(PpuThread& t, u32 id)
{
    const auto raw = find_raw(t.kernel.impl(), id, true);
    if (!raw) return err(CELL_ESRCH);
    raw->run_ctrl.store(SPU_RUNCNTL_RUN_REQUEST, std::memory_order_release);
    raw->start();
    return CELL_OK;
}

Result sys_isolated_spu_create_interrupt_tag(PpuThread& t, u32 id, u32 cls, u32, u32 intrtag)
{
    return create_tag(t, id, cls, intrtag, true);
}
Result sys_isolated_spu_set_int_mask(PpuThread& t, u32 id, u32 cls, u64 mask) { return set_mask(t, id, cls, mask, true); }
Result sys_isolated_spu_get_int_mask(PpuThread& t, u32 id, u32 cls, u32 out) { return get_ctrl(t, id, cls, out, true, true); }
Result sys_isolated_spu_set_int_stat(PpuThread& t, u32 id, u32 cls, u64 stat) { return set_stat(t, id, cls, stat, true); }
Result sys_isolated_spu_get_int_stat(PpuThread& t, u32 id, u32 cls, u32 out) { return get_ctrl(t, id, cls, out, true, false); }
Result sys_isolated_spu_read_puint_mb(PpuThread& t, u32 id, u32 out) { return read_puint_mb(t, id, out, true); }
Result sys_isolated_spu_set_spu_cfg(PpuThread& t, u32 id, u32 value) { return set_cfg(t, id, value, true); }
Result sys_isolated_spu_get_spu_cfg(PpuThread& t, u32 id, u32 out) { return get_cfg(t, id, out, true); }

// ------------------------------------------------------------------------------------------------- interrupt calls

// sys_interrupt_tag_create: tags come from the raw SPU calls; this answers CELL_OK, as RPCS3's null slot.
Result sys_interrupt_tag_create(PpuThread&) { return CELL_OK; }

// sys_interrupt_tag_destroy(intrtag): not while a handler is established on it.
Result sys_interrupt_tag_destroy(PpuThread& t, u32 intrtag)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto tag = s.objects.get<InterruptTag>(intrtag);
    if (!tag) return err(CELL_ESRCH);
    {
        RawSpus& r = raws(s);
        std::lock_guard lock(r.mutex);
        if (tag->handler) return err(CELL_EBUSY);
        if (tag->raw < kMaxRaw && r.raw[tag->raw] && r.raw[tag->raw]->int_ctrl[tag->cls].tag == tag) {
            r.raw[tag->raw]->int_ctrl[tag->cls].tag = nullptr;
        }
    }
    s.objects.remove(InterruptTag::kIdBase, intrtag);
    return CELL_OK;
}

// _sys_interrupt_thread_establish(u32* ih, intrtag, intrthread, arg1, arg2).
Result _sys_interrupt_thread_establish(PpuThread& t, u32 ih, u32 intrtag, u32 intrthread, u64 arg1, u64 arg2)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto tag = s.objects.get<InterruptTag>(intrtag);
    if (!tag) return err(CELL_ESRCH);
    const auto it = s.threads.get<PpuThread>(intrthread);
    if (!it) return err(CELL_ESRCH);
    if (!s.guest.valid(ih, 4)) return err(CELL_EFAULT);
    {
        std::lock_guard lock(it->lock);
        if (it->started || it->attached) return err(CELL_EAGAIN); // running: established on another tag
    }
    auto sv = std::make_shared<InterruptService>();
    sv->thread = it;
    sv->arg1 = arg1;
    sv->arg2 = arg2;
    sv->tag = tag;
    {
        RawSpus& r = raws(s);
        std::lock_guard lock(r.mutex);
        if (tag->handler) return err(CELL_ESTAT);
        const u32 id = s.objects.insert(InterruptService::kIdBase, sv);
        if (!id) return err(CELL_EAGAIN);
        sv->id = id;
        tag->handler = sv;
    }
    {
        std::lock_guard lock(it->lock);
        it->started = true;
        it->interrupt = true;
    }
    {
        std::lock_guard lock(s.host_threads->mutex);
        s.host_threads->live++;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, t.kernel.config().host_stack_size);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
#if defined(__APPLE__)
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INITIATED, 0);
#endif
    auto* args = new ServiceArgs{&t.kernel, sv};
    pthread_t host{};
    if (pthread_create(&host, &attr, service_main, args) != 0) {
        delete args;
        pthread_attr_destroy(&attr);
        {
            std::lock_guard lock(s.host_threads->mutex);
            s.host_threads->live--;
        }
        {
            RawSpus& r = raws(s);
            std::lock_guard lock(r.mutex);
            tag->handler = nullptr;
        }
        s.objects.remove(InterruptService::kIdBase, sv->id);
        return err(CELL_EAGAIN);
    }
    pthread_attr_destroy(&attr);
    s.guest.w32(ih, sv->id);
    return CELL_OK;
}

// sys_interrupt_thread_eoi(): the handler is done; its thread waits for the next interrupt.
void sys_interrupt_thread_eoi(PpuThread& t)
{
    if (!t.interrupt) {
        PS_LOG_ERROR(kRawChan, "sys_interrupt_thread_eoi on PPU thread {:#x}, which is not an interrupt thread", t.id);
        return;
    }
    throw InterruptReturn{};
}

// _sys_interrupt_thread_disestablish(ih, u64* r13): the thread ends; its TLS pointer comes back. As RPCS3, `ih` may
// also be an interrupt thread never established: it ends the same way.
Result _sys_interrupt_thread_disestablish(PpuThread& t, u32 ih, u32 r13)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto sv = s.objects.get<InterruptService>(ih);
    if (!sv) {
        const auto it = s.threads.get<PpuThread>(ih);
        if (!it || !it->interrupt) return err(CELL_ESRCH);
        {
            std::lock_guard lock(it->lock);
            if (it->started) return err(CELL_ESRCH);
            it->started = true;
        }
        if (!s.guest.put64(r13, it->ctx->gpr[13])) return err(CELL_EFAULT);
        thread_finish(*it, 0);
        return CELL_OK;
    }
    const u64 tls = sv->thread->ctx->gpr[13];
    end_service(s, sv);
    if (!s.guest.put64(r13, tls)) return err(CELL_EFAULT);
    return CELL_OK;
}

// Process exit: raw SPUs stop, interrupt threads end.
void abort_raw_spus(Kernel::Impl& s)
{
    if (!s.raw_spu_state) return;
    RawSpus& r = raws(s);
    std::vector<std::shared_ptr<RawSpu>> all;
    {
        std::lock_guard lock(r.mutex);
        for (const auto& raw : r.raw)
            if (raw) all.push_back(raw);
    }
    for (const auto& raw : all) {
        ps_spu_request_stop(&raw->ctx);
        for (IntCtrl& c : raw->int_ctrl) {
            std::shared_ptr<InterruptService> h;
            {
                std::lock_guard lock(r.mutex);
                if (c.tag) h = c.tag->handler;
            }
            if (h) {
                {
                    std::lock_guard lock(h->m);
                    h->quit = true;
                }
                h->cv.notify_all();
            }
        }
    }
}

void register_raw_spu_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 150, sys_raw_spu_create_interrupt_tag);
    PS_LV2_BIND(table, 151, sys_raw_spu_set_int_mask);
    PS_LV2_BIND(table, 152, sys_raw_spu_get_int_mask);
    PS_LV2_BIND(table, 153, sys_raw_spu_set_int_stat);
    PS_LV2_BIND(table, 154, sys_raw_spu_get_int_stat);
    PS_LV2_BIND(table, 160, sys_raw_spu_create);
    PS_LV2_BIND(table, 161, sys_raw_spu_destroy);
    PS_LV2_BIND(table, 163, sys_raw_spu_read_puint_mb);
    PS_LV2_BIND(table, 196, sys_raw_spu_set_spu_cfg);
    PS_LV2_BIND(table, 197, sys_raw_spu_get_spu_cfg);
    PS_LV2_BIND(table, 199, sys_raw_spu_recover_page_fault);
    PS_LV2_BIND(table, 230, sys_isolated_spu_create);
    PS_LV2_BIND(table, 231, sys_isolated_spu_destroy);
    PS_LV2_BIND(table, 232, sys_isolated_spu_start);
    PS_LV2_BIND(table, 233, sys_isolated_spu_create_interrupt_tag);
    PS_LV2_BIND(table, 234, sys_isolated_spu_set_int_mask);
    PS_LV2_BIND(table, 235, sys_isolated_spu_get_int_mask);
    PS_LV2_BIND(table, 236, sys_isolated_spu_set_int_stat);
    PS_LV2_BIND(table, 237, sys_isolated_spu_get_int_stat);
    PS_LV2_BIND(table, 238, sys_isolated_spu_set_spu_cfg);
    PS_LV2_BIND(table, 239, sys_isolated_spu_get_spu_cfg);
    PS_LV2_BIND(table, 240, sys_isolated_spu_read_puint_mb);
    PS_LV2_BIND(table, 80, sys_interrupt_tag_create);
    PS_LV2_BIND(table, 81, sys_interrupt_tag_destroy);
    PS_LV2_BIND(table, 84, _sys_interrupt_thread_establish);
    PS_LV2_BIND(table, 88, sys_interrupt_thread_eoi);
    PS_LV2_BIND(table, 89, _sys_interrupt_thread_disestablish);
}

} // namespace pspocket::lv2
