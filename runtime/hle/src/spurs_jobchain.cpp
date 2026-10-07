// The native job chain policy module (D-037 stage 5, D-055): runs a job chain's jobs on a worker the way Sony's job
// chain policy module does, without running it. The reference is libsre (fw 4.92: the module that
// cellSpursCreateJobChainWithAttribute registers, read on the Mac as the reference and never copied, D-046). RPCS3 has
// no job chain policy (its spursJobChainEntry is empty).
//
// What a job sees is libsre's contract. Its binary sits at a 1 KB page from LS 0x4C00 and is entered at its first byte
// with $3 = its context (CellSpursJobContext2 at 0x4940), $4 = the LS copy of its descriptor, $126 = the binary's LS
// address (job code is position independent) and $sp in a stack above its scratch buffer. Its input (gathered by its
// DMA list), read-only cache elements and output buffer are LS pages too. A job runs to completion and puts its own
// output with DMA on the context's tag. LS 0x80..0x17F stay free for job code, and the kernel context (0x1C0: the
// CellSpurs address) stays valid.
//
// The guest protocol is libsre's, so that several workers and the PPU see one consistent chain. Each visit ("grab")
// takes the chain's lock byte through the reservation system, edits a copy of the chain's first line, executes the
// commands at pc as they are in guest memory at that moment (the PPU rewrites END and JTS in place and kicks the chain
// again) and releases the lock, merging back what the PPU may change meanwhile (urgent command slots, isHalted, the
// grab limit). After every visit the workload's ready count is stored: busy SPUs + 1 for a chain with autoReadyCount,
// 0 at END, 1 while waiting at a SYNC. Binaries and cache elements stay in LS between jobs while the module stays on
// the SPU (libsre's LS cache), so a binary's data keeps its values from one job to the next.
//
// Deviations: jobs run one at a time, and a job's descriptor, binary and input are loaded when it starts (libsre
// fetches them while the previous job runs, unless that job has the stall-successor flag), so a job that reads what the
// previous job on the same SPU wrote gets the new data here. An SPU that leaves the chain after running jobs makes one
// more visit without taking jobs, so its busy bit doesn't hold up a SYNC elsewhere. A visit that changes nothing (a JTS
// waiting for the PPU) is followed by a short sleep instead of libsre's spin. Not implemented: the memory check (jobType
// bit 1, jobMemoryCheck: logged once, the jobs run without it, D-171); trace packets; the exception event handler
// (cellSpursJobChainSetExceptionEventHandler keeps one, but a job that faults stops with a fatal log).

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/core/reservation.h"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/spu/runtime.h"
#include "spu_programs.hpp"
#include "spurs_native.hpp"

namespace pspocket::hle::spurs {
namespace {

constexpr std::string_view kJobChan = "spurs";

// Commands (u64): a JOB is a descriptor EA (non-zero, low 3 bits 0); otherwise opcode = cmd & 7, and for opcode 7
// cmd & 0x7F. Addresses ride in the upper bits (EA | opcode), labels in the upper word.
constexpr u32 kOpJob = 0, kOpResetPc = 1, kOpSync = 2, kOpNext = 3, kOpCall = 4, kOpFlush = 5, kOpJobList = 6,
              kOpLong = 7;
constexpr u32 kSyncLabelBit = 0x08, kSyncLightBit = 0x10;
constexpr u32 kAbort = 0x07, kGuard = 0x0F, kSetLabel = 0x17, kRet = 0x77, kEnd = 0x7F;
constexpr u64 kJts = 0x0000000800000012ull; // jump to self: wait here until the PPU rewrites the command
// Any command with the JTS bit waits (D-125): the PPU rewrites a JTS with a JOB low word first, then the high word, so
// a command caught between the two stores (0x00000008_<job EA> in GTA V's escape) reads as a JTS until it is whole.
constexpr u64 kJtsBit = 0x0000000800000000ull;
static_assert(kJts & kJtsBit);

constexpr u32 kPage = 0x400;
constexpr u32 kRing = 4;              // descriptor buffers
constexpr u32 kMaxCache = 4;          // cache elements per job
constexpr u32 kCacheEntries = 8;      // LS cache
constexpr u32 kDefaultStack = 0x2000; // a descriptor's sizeStack 0
constexpr u32 kDmaPc = 0x4478;        // libsre's DMA helper, for messages about the module's own DMA
// A binary's word at +0x10 (the job binary marker libsre checks): `xori $80,$80,4` or `xori $80,$80,2`.
constexpr u32 kBinaryMarkerA = 0x44012850, kBinaryMarkerB = 0x4400A850;
// The registers libsre leaves in $80..$82 at a job's entry: where it saved its own $sp, $lr and $126.
constexpr u32 kSavedSp = 0x3FC70, kSavedLr = 0x3FC60, kSaved126 = 0x3FC50;
constexpr u32 CELL_SPURS_MODULE_POLL_STATUS_READYCOUNT = 1;

// What a visit left the chain in (libsre's grab flags): blocked (SYNC, LWSYNC, GUARD, END), ended (END).
constexpr u32 kGrabBlocked = 2, kGrabEnded = 1;

u32 pages_of(u32 bytes) { return std::max(1u, (bytes + kPage - 1) / kPage); }

// The job memory check (cellSpursJobChainAttributeSetJobTypeMemoryCheck sets the chain's jobMemoryCheck; a descriptor's
// jobType bit 1 asks for it per job) is libsre's debugging aid: the module watches the local store around a job's buffers
// and halts the chain with CELL_SPURS_JOB_ERROR_MEMORY_CORRUPTED when the job wrote outside them. It is not done
// natively (D-171): the jobs run as they would without it, so a job writing outside its buffers is not caught. Logged
// the first time.
void note_memory_check(std::string_view who)
{
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        PS_LOG_WARN(kJobChan, "{} asks for the job memory check: not done natively, the jobs run without it (logged once)",
            who);
    }
}

const char* job_error_name(u32 code)
{
    switch (code) {
    case CELL_SPURS_JOB_ERROR_PERM: return "PERM";
    case CELL_SPURS_JOB_ERROR_JOB_DESCRIPTOR: return "JOB_DESCRIPTOR";
    case CELL_SPURS_JOB_ERROR_JOB_DESCRIPTOR_SIZE: return "JOB_DESCRIPTOR_SIZE";
    case CELL_SPURS_JOB_ERROR_MEMORY_SIZE: return "MEMORY_SIZE";
    case CELL_SPURS_JOB_ERROR_UNKNOWN_COMMAND: return "UNKNOWN_COMMAND";
    case CELL_SPURS_JOB_ERROR_JOBLIST_ALIGNMENT: return "JOBLIST_ALIGNMENT";
    case CELL_SPURS_JOB_ERROR_JOB_ALIGNMENT: return "JOB_ALIGNMENT";
    case CELL_SPURS_JOB_ERROR_CALL_OVERFLOW: return "CALL_OVERFLOW";
    case CELL_SPURS_JOB_ERROR_ABORT: return "ABORT";
    case CELL_SPURS_JOB_ERROR_DMALIST_ELEMENT: return "DMALIST_ELEMENT";
    case CELL_SPURS_JOB_ERROR_NUM_CACHE: return "NUM_CACHE";
    case CELL_SPURS_JOB_ERROR_INVALID_BINARY: return "INVALID_BINARY";
    default: return "?";
    }
}

// A job taken from the chain: its descriptor's EA and size, and what the descriptor held when it was taken.
struct Job {
    u64 ea = 0;
    u32 size = 0;
    u64 check = 0;      // a hash of the descriptor at the grab (0: not read)
    u64 grabbed_ns = 0; // when
    std::array<u8, 0x30> head{}; // its header then
};

// A job's descriptor as guest memory holds it now: the hash of what the job uses (its header, input DMA list and
// cache list: the chain reads sizeJobDescriptor bytes, and GTA V's EDGE jobs put other jobs' output right after their
// own 0x200), and its header into head (when given). 0 when it is not in guest memory.
u64 descriptor_check(lv2::Kernel& kernel, u64 ea, u32 size, u8* head)
{
    if ((ea >> 32) || size < 0x30 || !kernel.memory().is_committed(static_cast<u32>(ea)) ||
        !kernel.memory().is_committed(static_cast<u32>(ea + 0x2F))) {
        return 0;
    }
    const u8* p = kernel.base() + static_cast<u32>(ea);
    size = std::min<u32>(std::min<u32>(size, 0x30 + ps_ld16(p + 0x0A) + ps_ld32(p + 0x24)), 0x800) & ~7u;
    if (!kernel.memory().is_committed(static_cast<u32>(ea + size - 1))) return 0;
    u64 h = 0xcbf29ce484222325ull;
    for (u32 i = 0; i < size; i += 8) {
        u64 w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
    }
    if (head) std::memcpy(head, p, 0x30);
    return h | 1;
}

u64 now_ns()
{
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::atomic<u64> g_descriptors_changed{0};

// n bytes at p + at as big-endian words, a space between them and two every 16 bytes.
std::string words(const u8* p, u32 at, u32 n)
{
    std::string s;
    for (u32 off = 0; off < n; off += 4) s += std::format("{}{:08x}", off == 0 ? "" : off % 16 ? " " : "  ", ps_ld32(p + at + off));
    return s;
}

// libsre's job heap: whole 1 KB pages from 0x4C00 up to the descriptor buffers, best fit among the gaps (the lowest
// address among equal gaps).
class Heap {
public:
    void reset(u32 end_page)
    {
        end_ = end_page;
        used_.clear();
    }
    // The first page of `pages` free pages, or 0.
    u32 alloc(u32 pages)
    {
        u32 best = 0, best_len = ~0u, at = ls::jc_heap / kPage;
        const auto gap = [&](u32 from, u32 to) {
            if (to - from >= pages && to - from < best_len) {
                best = from;
                best_len = to - from;
            }
        };
        for (const auto& [start, n] : used_) {
            gap(at, start);
            at = start + n;
        }
        gap(at, end_);
        if (best) used_.insert(std::lower_bound(used_.begin(), used_.end(), std::pair{best, 0u}), {best, pages});
        return best;
    }
    void free(u32 page)
    {
        const auto it = std::find_if(used_.begin(), used_.end(), [page](const auto& a) { return a.first == page; });
        PS_CHECK(it != used_.end());
        used_.erase(it);
    }
    // "first..last" of every allocation, for reports.
    std::string describe() const
    {
        std::string s = std::format("pages {}..{}:", ls::jc_heap / kPage, end_ - 1);
        for (const auto& [start, n] : used_) s += std::format(" {}..{}", start, start + n - 1);
        return s;
    }

private:
    u32 end_ = 0;
    std::vector<std::pair<u32, u32>> used_; // (first page, pages), sorted
};

// An LS cache entry: a job binary or a read-only cache element, by EA.
struct CacheEntry {
    u64 ea = 0;
    u32 size = 0; // bytes the allocation holds
    u32 page = 0; // 0: a free entry
    u32 pages = 0;
    u32 refs = 0; // jobs using it
    u64 used = 0; // LRU stamp
    // A binary's lifted program, identified when the binary was loaded (D-076).
    bool identified = false;
    const ps_spu_program* program = nullptr;
};

class JobChainPolicy {
public:
    JobChainPolicy(Worker& w, u32 wid, u32 jc)
        : w_(w), wid_(wid), jc_(jc), base_(w.sched.base), ls_(w.ls), me_(static_cast<u8>(1u << w.spu_num))
    {
    }

    void run();

private:
    struct Grab {
        std::vector<Job> jobs;
        u32 flags = 0;
        bool exit = false;    // the chain is halted, or this visit reported an error: leave the SPU
        bool changed = false; // the visit changed the chain (or took jobs)
    };

    void init();
    Grab grab();
    bool acquire();
    void release();
    void batch(const u64* cmds, u32 n, bool urgent, Grab& g, bool* list_loaded);
    bool command(u64 cmd, bool urgent, Grab& g, bool* list_loaded);
    bool guard(u64 ea, Grab& g);
    void release_guard_wait();
    void take_from_list(u32 slot, Grab& g);
    void note_grab(Job& job) const
    {
        job.check = descriptor_check(w_.sched.kernel, job.ea, job.size, job.head.data());
        job.grabbed_ns = now_ns();
    }
    void check_descriptor(const Job& job) const;
    u32 list_left(u32 slot) { return ps_read32(line_, jc_off::jobLists + 16 * slot); }
    void store_ready_count(const Grab& g);
    void advance(bool urgent)
    {
        if (!urgent) ps_write64(line_, jc_off::pc, ps_read64(line_, jc_off::pc) + 8);
    }
    void clear_my_bit(u32 offset) { ps_write8(line_, offset, static_cast<u8>(ps_read8(line_, offset) & ~me_)); }

    bool run_job(const Job& job);
    u32 alloc(u32 bytes);
    s32 cache_get(u64 ea, u32 bytes, u32 load, u32 tag);
    void cache_put(s32 entry);
    bool evict_one();
    void evict_unreferenced();
    void get(u32 lsa, u64 ea, u32 size, u32 tag, std::string_view what);
    void mfc(u32 cmd, u32 lsa, u64 ea, u32 size, u32 tag, std::string_view what);

    void fail(u32 code, u64 cause);
    [[noreturn]] void fatal(std::string_view what) const;
    // D-138: a descriptor found broken: what it held when read, what memory holds now, the SPU DMA writes over it and
    // the commands around the chain's pc.
    void report_descriptor(u64 cause) const;
    // The DMA log's tag (ps_spu_set_dma_tag) while the module itself runs: the workload, the chain.
    u64 policy_tag() const { return 0xFF00000000000000ull | (u64{wid_} << 48) | jc_; }

    Worker& w_;
    const u32 wid_;
    const u32 jc_;
    u8* const base_;
    u8* const ls_;
    const u8 me_; // this SPU's bit in the chain's SPU masks

    // The chain's line 0 while this SPU holds the lock (libsre's copy at LS 0x4A00).
    alignas(16) u8 line_[PS_RES_LINE_SIZE]{};
    bool urgent_taken_ = false; // this visit executed urgent slot 0 (the release pops it)
    bool may_take_ = false;     // the last poll reported the ready count for this SPU (libsre: LS 0x4A8B)
    u8 blocked_count_ = 0;      // the ready count to store when the visit blocks (libsre: LS 0x4A8D)

    // From init (libsre reads the chain's two lines when it enters).
    u32 max_size_ = 0;  // maxSizeJobDescriptor
    u32 desc_base_ = 0; // the descriptor buffers
    u8 tags_[2]{};
    u32 sdk_ = 0;
    u32 jobs_run_ = 0;
    u32 job_ea_ = 0; // the descriptor of the job being loaded or run, for fatal logs
    Job job_{}; // that job, as the grab took it
    u32 desc_ls_ = 0; // where in LS it was read
    Heap heap_;
    std::array<CacheEntry, kCacheEntries> cache_{};
    u64 stamp_ = 0;
    u32 stack_page_ = 0, stack_pages_ = 0; // scratch + stack, kept for the next job of the same size
};

[[noreturn]] void JobChainPolicy::fatal(std::string_view what) const
{
    // What the SPU read of the job at hand, what memory holds now, who wrote over it and the commands around pc (D-138):
    // a descriptor written over stops a chain on more paths than the job errors (run 100: a binary at EA 0x10).
    if (job_ea_) report_descriptor(job_ea_);
    // The descriptor of the job at hand as the PPU wrote it (header, DMA list and cache list), when there is one.
    std::string job;
    const u32 size = ps_read16(base_, jc_ + jc_off::sizeJobDescriptor);
    if (job_ea_ && size <= 0x800 && w_.sched.kernel.memory().is_committed(job_ea_) &&
        w_.sched.kernel.memory().is_committed(job_ea_ + size - 1)) {
        job = std::format("; descriptor {:#x}:", job_ea_);
        for (u32 off = 0; off < size; off += 4) job += std::format("{}{:08x}", off % 16 ? " " : "  ", ps_read32(base_, job_ea_ + off));
    }
    PS_FATAL(kJobChan, "SPURS {:#x} SPU {}, job chain {:#x} (workload {}): {} (pc {:#x}){}", w_.sched.spurs, w_.spu_num,
        jc_, wid_, what, ps_read64(base_, jc_ + jc_off::pc), job);
}

void JobChainPolicy::report_descriptor(u64 cause) const
{
    if (!job_ea_ || cause != job_ea_) return; // the error is not about the descriptor at hand
    const u32 size = std::min<u32>(ps_read16(base_, jc_ + jc_off::sizeJobDescriptor), 0x800);
    if (job_.ea == job_ea_ && job_.check) {
        PS_LOG_ERROR(kJobChan, "  descriptor {:#x} at the grab, {:.3f} ms ago: {}", job_ea_,
            static_cast<double>(now_ns() - job_.grabbed_ns) / 1e6, words(job_.head.data(), 0, 0x30));
    }
    PS_LOG_ERROR(kJobChan, "  descriptor {:#x} as the SPU read it: {}", job_ea_, words(ls_, desc_ls_, 0x30));
    PS_LOG_ERROR(kJobChan, "  descriptor {:#x} in memory now:      {}", job_ea_, words(base_, job_ea_, 0x30));
    std::array<ps_spu_dma_write, 12> w{};
    const u32 n = ps_spu_dma_writes_over(job_ea_, size, w.data(), static_cast<u32>(w.size()));
    const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    for (u32 i = 0; i < n; i++) {
        const u64 tag = w[i].tag;
        const std::string by = tag == 0 ? std::string("no SPURS work")
            : (tag >> 56) == 0xFF ? std::format("the policy of workload {} (job chain or SPURS {:#x})", (tag >> 48) & 0xFF,
                                        static_cast<u32>(tag))
                                  : std::format("job {:#x} (binary {:#x})", static_cast<u32>(tag), static_cast<u32>(tag >> 32));
        PS_LOG_ERROR(kJobChan, "  SPU DMA write over it: {:#x} bytes at {:#x} (MFC command {:#x}, SPU pc {:#x}) {:.3f} ms "
            "ago, by {}", w[i].size, w[i].ea, w[i].cmd, w[i].pc, static_cast<double>(now - w[i].time_ns) / 1e6, by);
    }
    if (!n) {
        PS_LOG_ERROR(kJobChan, "  no SPU DMA write over it among the last writes each thread keeps (PSPOCKET_DMA_LOG): the "
            "PPU or the RSX wrote it, an SPU before that, or the SPU read it before the game wrote it");
    }
    // The commands before and after the chain's pc (the JOB that named the descriptor is behind it).
    const u64 pc = ps_read64(base_, jc_ + jc_off::pc);
    std::string cmds;
    for (s32 k = -6; k <= 3; k++) {
        const u64 at = pc + static_cast<u64>(static_cast<s64>(k) * 8);
        if (at >> 32 || !w_.sched.kernel.memory().is_committed(static_cast<u32>(at))) continue;
        cmds += std::format("{}{:#x}: {:016x}", cmds.empty() ? "" : ", ", at, ps_read64(base_, static_cast<u32>(at)));
    }
    PS_LOG_ERROR(kJobChan, "  commands around pc {:#x}: {}", pc, cmds);
}

// A descriptor that changed between the grab and the job's start: the job runs with what memory holds now, which the
// game may have written for other work (the EDGE/GFX stops, improve.md). Counted, and the first ones logged.
void JobChainPolicy::check_descriptor(const Job& job) const
{
    if (!job.check) return;
    std::array<u8, 0x30> head{};
    const u64 now = descriptor_check(w_.sched.kernel, job.ea, job.size, head.data());
    if (now == job.check) return;
    if (g_descriptors_changed.fetch_add(1, std::memory_order_relaxed) >= 20) return;
    PS_LOG_WARN(kJobChan, "SPURS {:#x} SPU {}, job chain {:#x} (workload {}): the descriptor of job {:#x} changed between "
        "its grab and its start, {:.3f} ms later", w_.sched.spurs, w_.spu_num, jc_, wid_, job.ea,
        static_cast<double>(now_ns() - job.grabbed_ns) / 1e6);
    PS_LOG_WARN(kJobChan, "  at the grab: {}", words(job.head.data(), 0, 0x30));
    PS_LOG_WARN(kJobChan, "  at the start: {}", words(head.data(), 0, 0x30));
    std::array<ps_spu_dma_write, 8> w{};
    const u32 used = std::min<u32>(job.size, 0x30 + ps_ld16(job.head.data() + 0x0A) + ps_ld32(job.head.data() + 0x24));
    const u32 n = ps_spu_dma_writes_over(static_cast<u32>(job.ea), used, w.data(), static_cast<u32>(w.size()));
    for (u32 i = 0; i < n; i++) {
        if (w[i].time_ns < job.grabbed_ns) continue; // newest first; older ones were there at the grab
        PS_LOG_WARN(kJobChan, "  SPU write since the grab: {:#x} bytes at {:#x} (MFC command {:#x}, SPU pc {:#x}, tag {:#x})",
            w[i].size, w[i].ea, w[i].cmd, w[i].pc, w[i].tag);
    }
}

void JobChainPolicy::init()
{
    const u8 flags = ps_read8(base_, jc_ + jc_off::flags);
    max_size_ = ((flags & jc_off::flagMaxSizeMask) << 3) + 0x100;
    desc_base_ = ls::jc_descriptors_end - ((kRing * max_size_ + kPage - 1) & ~(kPage - 1));
    heap_.reset(desc_base_ / kPage);
    tags_[0] = ps_read8(base_, jc_ + jc_off::tag1);
    tags_[1] = ps_read8(base_, jc_ + jc_off::tag2);
    sdk_ = ps_read32(base_, jc_ + jc_off::sdkVer);
    if (ps_read8(base_, jc_ + jc_off::jobMemoryCheck)) note_memory_check("the chain (cellSpursJobChainAttributeSetJobTypeMemoryCheck)");
    // A job returns into a `stop` the worker takes (libsre: the instruction after its call).
    ps_write32(ls_, ls::jc_return, kJobReturnStop);
}

void JobChainPolicy::run()
{
    init();
    // libsre ignores the dispatch's poll status: its first visit takes no jobs and only stores the ready count; jobs
    // flow after a poll reports the ready count for this SPU.
    may_take_ = false;
    auto pause = std::chrono::microseconds(0);
    for (;;) {
        if (w_.sched.stopping.load(std::memory_order_relaxed)) return;
        const bool could_take = may_take_;
        const Grab g = grab();
        if (g.exit) return;
        for (const Job& job : g.jobs) {
            if (!run_job(job)) return;
        }
        const Worker::Selection sel = w_.select(true);
        if (sel.wid != wid_) {
            // Another workload (or none) should have this SPU. libsre drains its jobs and leaves; the last visit
            // without taking jobs clears this SPU's busy bit, so a SYNC elsewhere doesn't wait for it.
            if (!g.jobs.empty()) {
                may_take_ = false;
                grab();
            }
            return;
        }
        may_take_ = (sel.poll_status & CELL_SPURS_MODULE_POLL_STATUS_READYCOUNT) != 0;
        // A visit that could take jobs but changed nothing (a JTS, commands waiting for the PPU) would only repeat:
        // libsre spins on it; here the worker sleeps a little, longer each time, then looks again.
        if (could_take && !g.changed) {
            pause = std::min<std::chrono::microseconds>(std::max(pause * 2, std::chrono::microseconds(50)),
                std::chrono::microseconds(1000));
            std::this_thread::sleep_for(pause);
        } else {
            pause = std::chrono::microseconds(0);
        }
    }
}

// ------------------------------------------------------------------------------------------------- the chain

// libsre's acquire: wait for the chain's lock byte (0x29), take it and copy line 0. False when the chain is halted
// (the module leaves at once) or the scheduler stops.
bool JobChainPolicy::acquire()
{
    for (;;) {
        bool halted = false, locked = false;
        reservation_op(base_, jc_, [&](u8* line) {
            halted = ps_read8(line, jc_off::isHalted) != 0;
            locked = !halted && ps_read8(line, jc_off::lock) != 0;
            if (halted || locked) return false;
            ps_write8(line, jc_off::lock, 1);
            std::memcpy(line_, line, sizeof line_);
            return true;
        });
        if (halted) return false;
        if (!locked) break;
        if (w_.sched.stopping.load(std::memory_order_relaxed)) return false;
        std::this_thread::yield();
    }
    urgent_taken_ = false;
    return true;
}

// libsre's release: store the line copy and unlock, keeping what the PPU may have changed meanwhile: the urgent command
// slots (less the one this visit executed), isHalted and maxGrabbedJob.
void JobChainPolicy::release()
{
    reservation_op(base_, jc_, [&](u8* line) {
        alignas(16) u8 out[PS_RES_LINE_SIZE];
        std::memcpy(out, line_, sizeof out);
        for (u32 k = 0; k < 4; k++) {
            const u32 from = urgent_taken_ ? k + 1 : k;
            ps_write64(out, jc_off::urgentCmds + 8 * k, from < 4 ? ps_read64(line, jc_off::urgentCmds + 8 * from) : 0);
        }
        ps_write8(out, jc_off::isHalted, ps_read8(line, jc_off::isHalted));
        ps_write16(out, jc_off::maxGrabbedJob, ps_read16(line, jc_off::maxGrabbedJob));
        ps_write8(out, jc_off::lock, 0);
        std::memcpy(line, out, sizeof out);
        return true;
    });
}

// One visit (libsre's grab): bookkeeping for this SPU, then urgent slot 0, the active job list, or the commands at
// pc; the ready count; release. Jobs run between visits, so this SPU never has jobs in flight here.
JobChainPolicy::Grab JobChainPolicy::grab()
{
    Grab g;
    if (!acquire()) {
        g.exit = true;
        return g;
    }
    alignas(16) u8 before[PS_RES_LINE_SIZE];
    std::memcpy(before, line_, sizeof before);

    // Another SPU lowered the ready count: no jobs until this SPU's next poll.
    if (ps_read8(line_, jc_off::lowered) & me_) {
        clear_my_bit(jc_off::lowered);
        may_take_ = false;
    }
    // No jobs in flight: this SPU is no longer busy, and the label job of its last batch (if any) is done.
    clear_my_bit(jc_off::labelRequest);
    clear_my_bit(jc_off::busy);
    clear_my_bit(jc_off::labelPending);
    blocked_count_ = ps_read8(line_, jc_off::readyCount);

    const u32 urgent_list = ps_read8(line_, jc_off::urgentList);
    bool urgent_pending = false;
    for (u32 k = 0; k < 4; k++) urgent_pending |= ps_read64(line_, jc_off::urgentCmds + 8 * k) != 0;
    if (!urgent_list && urgent_pending) {
        if (may_take_) {
            if (ps_read8(line_, jc_off::flags) & jc_off::flagGuardWait) release_guard_wait();
            const u64 cmd = ps_read64(line_, jc_off::urgentCmds);
            urgent_taken_ = true;
            batch(&cmd, 1, true, g, nullptr);
        }
    } else if (list_left(urgent_list)) {
        if (may_take_) take_from_list(urgent_list, g);
    } else {
        // The commands at pc, as the PPU left them, up to the end of their line and the grab limit.
        const u64 pc = ps_read64(line_, jc_off::pc);
        const u32 line_ea = static_cast<u32>(pc) & ~0x7Fu;
        if ((pc >> 32) || !w_.sched.kernel.memory().is_committed(line_ea)) {
            fatal(std::format("the chain's pc {:#x} is not in guest memory (an MFC fault on the PS3)", pc));
        }
        alignas(16) u8 cmds_line[PS_RES_LINE_SIZE];
        ps_res_acquire(base_, line_ea, cmds_line);
        const u32 first = (static_cast<u32>(pc) & 0x7F) / 8;
        const u32 n = std::min<u32>(16 - first, ps_read16(line_, jc_off::maxGrabbedJob));
        u64 cmds[16];
        for (u32 i = 0; i < n; i++) cmds[i] = ps_read64(cmds_line, 8 * (first + i));
        bool list_loaded = false;
        batch(cmds, n, false, g, &list_loaded);
        // A job list just started: take from it at once.
        if (!g.exit && list_loaded && g.jobs.empty() && may_take_) {
            take_from_list(ps_read8(line_, jc_off::urgentList), g);
        }
    }
    if (g.exit) return g; // an error was reported: libsre leaves with the lock taken; the chain is halted

    if (!g.jobs.empty()) ps_write8(line_, jc_off::busy, static_cast<u8>(ps_read8(line_, jc_off::busy) | me_));
    if (ps_read8(line_, jc_off::flush) & me_) {
        evict_unreferenced();
        clear_my_bit(jc_off::flush);
    }
    store_ready_count(g);
    g.changed = !g.jobs.empty() || std::memcmp(before, line_, sizeof before) != 0;
    release();
    return g;
}

// The fetched commands in order (libsre's batch): JOBs are collected while this SPU may take jobs (else the batch
// ends at the first one); any other command executes, and a jump, a wait or the end of the chain ends the batch.
void JobChainPolicy::batch(const u64* cmds, u32 n, bool urgent, Grab& g, bool* list_loaded)
{
    for (u32 i = 0; i < n && !g.exit; i++) {
        const u64 cmd = cmds[i];
        if (cmd & kJtsBit) return; // a JTS, or a command the PPU is still writing: the batch ends; pc stays
        if (cmd != 0 && (cmd & 7) == kOpJob) {
            if (!may_take_) return;
            g.jobs.push_back({cmd, ps_read16(line_, jc_off::sizeJobDescriptor)});
            note_grab(g.jobs.back());
            advance(urgent);
            continue;
        }
        if (!command(cmd, urgent, g, list_loaded)) return;
    }
}

// One command other than JOB (libsre's command function). False when the batch ends there.
bool JobChainPolicy::command(u64 cmd, bool urgent, Grab& g, bool* list_loaded)
{
    const u64 pc = ps_read64(line_, jc_off::pc);
    const u32 low = static_cast<u32>(cmd);
    const u64 ea = cmd & ~7ull;
    const auto unknown = [&] {
        if (urgent) fatal(std::format("urgent command {:#018x} is not valid there (libsre halts the SPU)", cmd));
        fail(CELL_SPURS_JOB_ERROR_UNKNOWN_COMMAND, pc);
        g.exit = true;
        return false;
    };
    const auto set_pc = [&](u64 to) { ps_write64(line_, jc_off::pc, to); };
    if (cmd & kJtsBit) return false; // a JTS (or one being written): the batch ends; pc stays
    switch (low & 7) {
    case kOpJob: // NOP
        advance(urgent);
        return true;
    case kOpResetPc:
        set_pc(ea);
        for (u32 k = 0; k < 3; k++) ps_write64(line_, jc_off::linkRegister + 8 * k, 0);
        ps_write8(line_, jc_off::urgentList, 0);
        std::memset(line_ + jc_off::jobLists, 0, 32);
        return false;
    case kOpSync: {
        if (low & ((low & (kSyncLabelBit | kSyncLightBit)) ? ~0x1Fu : ~7u)) return unknown();
        const bool passes = (low & kSyncLabelBit) ? ps_read8(line_, jc_off::labelPending) == 0
                                                  : ps_read8(line_, jc_off::busy) == 0 && g.jobs.empty();
        if (passes) {
            advance(urgent);
            return true;
        }
        // Blocked: pc stays; a full SYNC asks for one SPU, a light one keeps the ready count.
        g.flags |= kGrabBlocked;
        if (!(low & kSyncLightBit)) blocked_count_ = 1;
        return false;
    }
    case kOpNext:
        if (urgent) fatal(std::format("NEXT {:#x} as an urgent command (libsre halts the SPU)", ea));
        set_pc(ea);
        return false;
    case kOpCall: {
        if (ps_read8(line_, jc_off::urgentList)) fatal(std::format("CALL {:#x} inside an urgent call (libsre halts the SPU)", ea));
        if (ps_read64(line_, jc_off::linkRegister)) {
            fail(CELL_SPURS_JOB_ERROR_CALL_OVERFLOW, pc);
            g.exit = true;
            return false;
        }
        ps_write64(line_, jc_off::linkRegister, ps_read64(line_, jc_off::linkRegister + 8));
        ps_write64(line_, jc_off::linkRegister + 8, ps_read64(line_, jc_off::linkRegister + 16));
        ps_write64(line_, jc_off::linkRegister + 16, urgent ? pc : pc + 8);
        ps_write8(line_, jc_off::urgentList, urgent ? 1 : 0);
        set_pc(ea);
        return false;
    }
    case kOpFlush:
        if (low & ~7u) return unknown();
        ps_write8(line_, jc_off::flush, 0xFF);
        advance(urgent);
        return true;
    case kOpJobList: {
        if (!ea) fatal("JOBLIST with a null list (libsre halts the SPU)");
        if (ea & 15) {
            fail(CELL_SPURS_JOB_ERROR_JOBLIST_ALIGNMENT, ea);
            g.exit = true;
            return false;
        }
        if ((ea >> 32) || !w_.sched.kernel.memory().is_committed(static_cast<u32>(ea))) {
            fatal(std::format("JOBLIST {:#x} is not in guest memory (an MFC fault on the PS3)", ea));
        }
        // CellSpursJobList {u32 numJobs, u32 sizeOfJob, u64 eaJobList} into the active slot of the line copy.
        const u32 slot = urgent ? 1 : ps_read8(line_, jc_off::urgentList);
        std::memcpy(line_ + jc_off::jobLists + 16 * slot, base_ + static_cast<u32>(ea), 16);
        if (urgent) ps_write8(line_, jc_off::urgentList, 1);
        else advance(false);
        if (list_loaded) *list_loaded = true;
        return false;
    }
    case kOpLong:
        break;
    }
    const u32 op = low & 0x7F;
    if (op == kGuard) return guard(cmd & ~0x7Full, g);
    if (low & ~0x7Fu) return unknown();
    switch (op) {
    case kAbort:
        fail(CELL_SPURS_JOB_ERROR_ABORT, pc);
        g.exit = true;
        return false;
    case kSetLabel:
        // One label: every SPU busy now must finish its jobs (and this batch's jobs) before a SYNC_LABEL passes.
        ps_write8(line_, jc_off::labelRequest, ps_read8(line_, jc_off::busy));
        ps_write8(line_, jc_off::labelPending,
            static_cast<u8>(ps_read8(line_, jc_off::busy) | (g.jobs.empty() ? 0 : me_)));
        advance(urgent);
        return false;
    case kRet: {
        const u64 to = ps_read64(line_, jc_off::linkRegister + 16);
        if (!to) break; // an empty call stack: END
        set_pc(to);
        ps_write64(line_, jc_off::linkRegister + 16, ps_read64(line_, jc_off::linkRegister + 8));
        ps_write64(line_, jc_off::linkRegister + 8, ps_read64(line_, jc_off::linkRegister));
        ps_write64(line_, jc_off::linkRegister, 0);
        ps_write8(line_, jc_off::urgentList, 0);
        return false;
    }
    case kEnd:
        break;
    default:
        return unknown();
    }
    // END (or RET with an empty call stack): pc stays on it, the ready count goes to 0.
    g.flags |= kGrabBlocked | kGrabEnded;
    blocked_count_ = 0;
    return false;
}

// GUARD (CellSpursJobGuard: ncount0 0x00, ncount1 0x04, job chain 0x08, requestSpuCount 0x10, autoReset 0x20, "the
// chain waits on it" 0x30): in one reservation, note whether the chain waits and reset a passed guard.
bool JobChainPolicy::guard(u64 ea, Grab& g)
{
    if ((ea >> 32) || !w_.sched.kernel.memory().is_committed(static_cast<u32>(ea))) {
        fatal(std::format("GUARD {:#x} is not in guest memory (an MFC fault on the PS3)", ea));
    }
    u32 count = 0;
    u8 request = 0;
    reservation_op(base_, static_cast<u32>(ea), [&](u8* gl) {
        count = ps_read32(gl, 0x00);
        request = static_cast<u8>(ps_read32(gl, 0x10));
        ps_write32(gl, 0x30, count != 0 ? 1u : 0u);
        if (ps_read32(gl, 0x20) && count == 0) ps_write32(gl, 0x00, ps_read32(gl, 0x04));
        return true;
    });
    const u8 flags = ps_read8(line_, jc_off::flags);
    if (count) {
        // Blocked: pc stays, the ready count goes to 0; cellSpursJobGuardNotify kicks the chain again.
        ps_write8(line_, jc_off::flags, static_cast<u8>(flags | jc_off::flagGuardWait));
        g.flags |= kGrabBlocked;
        blocked_count_ = 0;
        return false;
    }
    if (!ps_read8(line_, jc_off::autoReadyCount)) {
        ps_write8(line_, jc_off::initSpuCount, request);
        may_take_ = false;
    }
    ps_write8(line_, jc_off::flags, static_cast<u8>(flags & ~jc_off::flagGuardWait));
    advance(false);
    return false; // commands after the guard were read before it passed: read them again
}

// An urgent command preempts a guard the chain waits on: the guard no longer has a waiter.
void JobChainPolicy::release_guard_wait()
{
    const u64 pc = ps_read64(line_, jc_off::pc);
    const u64 cmd = ps_read64(base_, static_cast<u32>(pc));
    if ((cmd & 0x7F) == kGuard && (static_cast<u32>(cmd) & 7) == kOpLong) {
        ps_write32(base_, static_cast<u32>(cmd & ~0x7Full) + 0x30, 0);
    }
    ps_write8(line_, jc_off::flags, static_cast<u8>(ps_read8(line_, jc_off::flags) & ~jc_off::flagGuardWait));
}

// libsre's job list grab: the next min(left, maxGrabbedJob) descriptors, contiguous at sizeOfJob strides.
void JobChainPolicy::take_from_list(u32 slot, Grab& g)
{
    const u32 at = jc_off::jobLists + 16 * slot;
    const u32 left = ps_read32(line_, at), stride = ps_read32(line_, at + 4);
    const u64 next = ps_read64(line_, at + 8);
    const u32 n = std::min<u32>(left, ps_read16(line_, jc_off::maxGrabbedJob));
    for (u32 i = 0; i < n; i++) {
        g.jobs.push_back({next + u64{i} * stride, stride});
        note_grab(g.jobs.back());
    }
    ps_write32(line_, at, left - n);
    ps_write64(line_, at + 8, next + u64{n} * stride);
    if (slot == 1 && left == n) ps_write8(line_, jc_off::urgentList, 0); // the urgent list is done
}

// The workload's ready count after a visit (libsre): while blocked, what the blocking command set; without
// autoReadyCount the requested SPU count; else the SPUs with jobs in flight + 1 (a higher count stays while jobs are
// taken). A lower count makes the other SPUs stop taking jobs until their next poll.
void JobChainPolicy::store_ready_count(const Grab& g)
{
    const u8 current = ps_read8(line_, jc_off::readyCount);
    u8 count;
    if (g.flags & kGrabBlocked) {
        count = blocked_count_;
    } else if (!ps_read8(line_, jc_off::autoReadyCount)) {
        count = ps_read8(line_, jc_off::initSpuCount);
    } else {
        const u8 busy = static_cast<u8>(std::popcount(ps_read8(line_, jc_off::busy)) + 1);
        count = current > busy && !g.jobs.empty() ? current : busy;
    }
    if (count == current) return;
    reservation_op(base_, w_.sched.spurs, [&](u8* line) {
        ps_write8(line, wkl_ready(wid_), count);
        std::memcpy(ls_ + ls::kernel, line, PS_RES_LINE_SIZE); // libsre updates it through the kernel's line buffer
        return true;
    });
    if (count < current) ps_write8(line_, jc_off::lowered, static_cast<u8>(ps_read8(line_, jc_off::lowered) | ~me_));
    ps_write8(line_, jc_off::readyCount, count);
}

// libsre's error path: the chain halts (isHalted), the workload shuts down unless the chain halts on errors, and the
// error and its cause go to jc+0x80 / +0x88. With haltOnError the SPU halts: fatal here. The module then leaves the
// SPU; a lock it holds stays taken (other SPUs leave when they see isHalted).
void JobChainPolicy::fail(u32 code, u64 cause)
{
    bool first = false;
    reservation_op(base_, jc_, [&](u8* line) {
        first = !ps_read8(line, jc_off::isHalted);
        if (first) ps_write8(line, jc_off::isHalted, 1);
        return first;
    });
    const bool halt = (ps_read8(base_, jc_ + jc_off::flags) & jc_off::flagHaltOnError) != 0;
    if (first) {
        if (!halt) {
            // cellSpursShutdownWorkload for a running workload: SHUTTING_DOWN and the system service messages.
            reservation_op(base_, w_.sched.spurs + 0x80, [&](u8* line) { // state1 and state2 are both in line 1
                const u8 state = ps_read8(line, wkl_state(wid_) - 0x80);
                if (state == SPURS_WKL_STATE_SHUTTING_DOWN || state == SPURS_WKL_STATE_REMOVABLE) return false;
                ps_write8(line, wkl_state(wid_) - 0x80, SPURS_WKL_STATE_SHUTTING_DOWN);
                ps_write8(line, wkl_off::sysSrvMsgUpdateWorkload - 0x80, 0xFF);
                return true;
            });
            reservation_op(base_, w_.sched.spurs, [](u8* line) {
                ps_write8(line, wkl_off::sysSrvMessage, 0xFF);
                return true;
            });
        }
        ps_write32(base_, jc_ + jc_off::error, code);
        ps_write32(base_, jc_ + jc_off::error + 4, 0);
        ps_write64(base_, jc_ + jc_off::cause, cause);
    }
    PS_LOG_ERROR(kJobChan, "SPURS {:#x} SPU {}, job chain {:#x} (workload {}): CELL_SPURS_JOB_ERROR_{} ({:#x}), cause {:#x}; "
        "the chain halts{}", w_.sched.spurs, w_.spu_num, jc_, wid_, job_error_name(code), code, cause,
        first ? "" : " (another SPU reported first)");
    report_descriptor(cause);
    if (halt) fatal("the chain halts on errors (haltOnError): libsre halts the SPU");
}

// ------------------------------------------------------------------------------------------------- jobs

// The module's own DMA through the worker's MFC (synchronous), split as libsre's DMA helper does: naturally aligned
// pieces of 1, 2, 4 or 8 bytes and 16-byte multiples of at most 16 KB.
void JobChainPolicy::get(u32 lsa, u64 ea, u32 size, u32 tag, std::string_view what)
{
    while (size) {
        u32 piece;
        if (!(ea & 15) && !(lsa & 15) && size >= 16) piece = std::min<u32>(size & ~15u, 0x4000);
        else piece = std::min<u32>(1u << std::countr_zero(static_cast<u32>(ea | 8)), std::bit_floor(std::min(size, 8u)));
        mfc(PS_MFC_GETF, lsa, ea, piece, tag, what);
        lsa += piece;
        ea += piece;
        size -= piece;
    }
}

void JobChainPolicy::mfc(u32 cmd, u32 lsa, u64 ea, u32 size, u32 tag, std::string_view what)
{
    SPUContext& c = w_.ctx;
    if (!(cmd & 4) && size && ((ea >> 32) || !w_.sched.kernel.memory().is_committed(static_cast<u32>(ea)) ||
                                  !w_.sched.kernel.memory().is_committed(static_cast<u32>(ea + size - 1)))) {
        fatal(std::format("{}: {:#x} bytes at {:#x} are not in guest memory (an MFC fault on the PS3)", what, size, ea));
    }
    ps_spu_wrch(&c, kDmaPc, PS_MFC_LSA, lsa);
    ps_spu_wrch(&c, kDmaPc, PS_MFC_EAH, static_cast<u32>(ea >> 32));
    ps_spu_wrch(&c, kDmaPc, PS_MFC_EAL, static_cast<u32>(ea));
    ps_spu_wrch(&c, kDmaPc, PS_MFC_Size, size);
    ps_spu_wrch(&c, kDmaPc, PS_MFC_TagID, tag);
    if (ps_spu_wrch(&c, kDmaPc, PS_MFC_Cmd, cmd)) {
        fatal(std::format("{}: MFC command {:#x} (LS {:#x}, EA {:#x}, {:#x} bytes, tag {}) faulted (exception {}, {:#x})",
            what, cmd, lsa, ea, size, tag, c.exception, c.stop_code));
    }
}

// Pages for `bytes`, evicting unreferenced LS cache entries (least recently used first) while nothing fits. 0: none.
u32 JobChainPolicy::alloc(u32 bytes)
{
    for (;;) {
        if (const u32 page = heap_.alloc(pages_of(bytes))) return page;
        if (!evict_one()) return 0;
    }
}

bool JobChainPolicy::evict_one()
{
    CacheEntry* lru = nullptr;
    for (CacheEntry& e : cache_) {
        if (e.page && !e.refs && (!lru || e.used < lru->used)) lru = &e;
    }
    if (!lru) return false;
    heap_.free(lru->page);
    *lru = CacheEntry{};
    return true;
}

void JobChainPolicy::evict_unreferenced()
{
    while (evict_one()) {
    }
}

// A binary or cache element through the LS cache: an entry for `ea` holding at least `bytes`, or a new one with the
// first `load` bytes fetched. Returns the entry, or -1 when no memory is left.
s32 JobChainPolicy::cache_get(u64 ea, u32 bytes, u32 load, u32 tag)
{
    for (u32 i = 0; i < kCacheEntries; i++) {
        CacheEntry& e = cache_[i];
        if (e.page && e.ea == ea && e.size >= bytes) {
            e.refs++;
            e.used = ++stamp_;
            return static_cast<s32>(i);
        }
    }
    const auto free_entry = [&]() -> s32 {
        for (u32 i = 0; i < kCacheEntries; i++) {
            if (!cache_[i].page) return static_cast<s32>(i);
        }
        return -1;
    };
    s32 slot = free_entry();
    if (slot < 0 && evict_one()) slot = free_entry();
    if (slot < 0) return -1;
    const u32 page = alloc(bytes);
    if (!page) return -1;
    slot = free_entry(); // eviction above may have freed a lower slot; any free one will do
    cache_[static_cast<u32>(slot)] = CacheEntry{ea, pages_of(bytes) * kPage, page, pages_of(bytes), 1, ++stamp_};
    get(page * kPage, ea, load, tag, "job binary or cache element");
    return slot;
}

void JobChainPolicy::cache_put(s32 entry)
{
    if (entry >= 0 && cache_[static_cast<u32>(entry)].refs) cache_[static_cast<u32>(entry)].refs--;
}

// One job, from its descriptor to its return (libsre's pipeline stages for one job, in order). False when the module
// leaves the SPU: an error, or the scheduler stops.
bool JobChainPolicy::run_job(const Job& job)
{
    // Grab stage: the descriptor size; prefetch: its alignment, then the descriptor into the next buffer.
    if (!(job.size == 64 || job.size == 128 || (job.size % 128 == 0 && job.size >= 256 && job.size <= max_size_))) {
        fail(CELL_SPURS_JOB_ERROR_JOB_DESCRIPTOR_SIZE, job.ea);
        return false;
    }
    if (job.ea & 15) {
        fail(CELL_SPURS_JOB_ERROR_JOB_ALIGNMENT, job.ea);
        return false;
    }
    job_ea_ = job.ea;
    job_ = job;
    check_descriptor(job);
    const u32 desc = desc_base_ + (jobs_run_ % kRing) * max_size_;
    desc_ls_ = desc;
    const u32 tag = tags_[jobs_run_ % 2]; // jobs alternate between the chain's two tags
    jobs_run_++;
    get(desc, job.ea, job.size, tags_[jobs_run_ % 2], "job descriptor");

    // Load stage: the binary, the input buffer and its DMA list, the cache elements.
    const u32 ea_word = ps_read32(ls_, desc + 0x04);
    const u32 bss = ps_read16(ls_, desc + 0x02) * 16u;
    const u32 size_binary = ps_read16(ls_, desc + 0x08) * 16u;
    const u32 size_dma_list = ps_read16(ls_, desc + 0x0A);
    const u32 ea_high_input = ps_read32(ls_, desc + 0x0C);
    const u32 size_in = ps_read32(ls_, desc + 0x14);
    const u32 size_out = ps_read32(ls_, desc + 0x18);
    const u32 size_stack = ps_read16(ls_, desc + 0x1C) ? ps_read16(ls_, desc + 0x1C) * 16u : kDefaultStack;
    const u32 size_scratch = ps_read16(ls_, desc + 0x1E) * 16u;
    const u32 ea_high_cache = ps_read32(ls_, desc + 0x20);
    const u32 size_cache_list = ps_read32(ls_, desc + 0x24);
    const u8 job_type = ps_read8(ls_, desc + 0x2C);
    if (!(ea_word & ~3u) || !size_binary || (ea_word & 0xC)) {
        fail(CELL_SPURS_JOB_ERROR_INVALID_BINARY, job.ea);
        return false;
    }
    const u32 num_cache = size_cache_list / 8;
    if (num_cache > kMaxCache) {
        fail(CELL_SPURS_JOB_ERROR_NUM_CACHE, job.ea);
        return false;
    }
    const u32 cache_list = desc + 0x30 + size_dma_list;
    for (u32 k = 0; k < num_cache; k++) {
        if (ps_read32(ls_, cache_list + 8 * k) & 7) {
            fail(CELL_SPURS_JOB_ERROR_DMALIST_ELEMENT, job.ea);
            return false;
        }
    }
    if ((size_dma_list & 7) || size_dma_list > 0x4000) {
        fail(CELL_SPURS_JOB_ERROR_DMALIST_ELEMENT, job.ea);
        return false;
    }
    if (job_type & 2) note_memory_check(std::format("job {:#x} (jobType bit 1)", job.ea));

    // The previous job's scratch + stack stays for this one only when it is the same size; libsre frees it after the
    // previous job otherwise, before this job's buffers are allocated.
    const u32 frame_pages = pages_of(size_scratch + size_stack);
    if (stack_page_ && frame_pages != stack_pages_) {
        heap_.free(stack_page_);
        stack_page_ = stack_pages_ = 0;
    }

    const bool no_cache = ea_word & 1;
    const u32 binary_ea = ea_word & ~3u;
    s32 binary_entry = -1;
    u32 binary_page = 0, in_page = 0, out_page = 0;
    std::array<s32, kMaxCache> cache_entries{};
    std::array<u32, kMaxCache> cache_lsa{};
    // The binary, the input buffer, the cache elements, then (libsre's run stage) the output buffer and scratch + stack
    // (kept from the previous job when the size is the same). Returns the buffer there was no room for, or "".
    const auto load = [&]() -> std::string_view {
        cache_entries.fill(-1);
        cache_lsa.fill(0);
        if (no_cache) {
            if (!(binary_page = alloc(size_binary + bss))) return "binary";
            get(binary_page * kPage, binary_ea, size_binary, tag, "job binary");
        } else {
            if ((binary_entry = cache_get(binary_ea, size_binary + bss, size_binary, tag)) < 0) return "binary";
            binary_page = cache_[static_cast<u32>(binary_entry)].page;
        }
        if (size_in && !(in_page = alloc(size_in))) return "input buffer";
        for (u32 k = 0; k < num_cache; k++) {
            const u32 size = ps_read32(ls_, cache_list + 8 * k);
            const u64 ea = (u64{ea_high_cache} << 32) | ps_read32(ls_, cache_list + 8 * k + 4);
            if ((cache_entries[k] = cache_get(ea, size, size, tag)) < 0) return "cache element";
            cache_lsa[k] = cache_[static_cast<u32>(cache_entries[k])].page * kPage;
        }
        if (size_out && !(out_page = alloc(size_out))) return "output buffer";
        if (!stack_page_) {
            if (!(stack_page_ = alloc(frame_pages * kPage))) return "scratch + stack";
            stack_pages_ = frame_pages;
        }
        return "";
    };
    const auto release_buffers = [&] {
        if (out_page) heap_.free(out_page);
        if (in_page) heap_.free(in_page);
        for (const s32 e : cache_entries) cache_put(e);
        if (no_cache && binary_page) {
            std::memset(ls_ + binary_page * kPage, 0, 16); // libsre drops a private binary and clears its marker
            heap_.free(binary_page);
        }
        cache_put(binary_entry);
        cache_entries.fill(-1);
        binary_entry = -1;
        binary_page = in_page = out_page = 0;
    };
    std::string_view missing = load();
    if (!missing.empty()) {
        // No room as the heap is laid out (a cached binary in the middle of the pages, say). libsre makes room by
        // rolling back what it fetched ahead for the next jobs; nothing is fetched ahead here, so the job is laid out
        // again in an emptied heap: every cached binary and cache element and the kept scratch + stack go (they are
        // loaded again when needed). Jobs are made to fit the heap on their own.
        release_buffers();
        evict_unreferenced();
        if (stack_page_) {
            heap_.free(stack_page_);
            stack_page_ = stack_pages_ = 0;
        }
        missing = load();
    }
    if (!missing.empty()) {
        PS_LOG_ERROR(kJobChan, "SPURS {:#x} SPU {}: job {:#x} needs pages for its binary {:#x} ({}), input {}, {} cache "
            "elements, output {}, scratch + stack {}: no room for its {} even in an emptied heap ({})", w_.sched.spurs,
            w_.spu_num, job.ea, binary_ea, pages_of(size_binary + bss), size_in ? pages_of(size_in) : 0, num_cache,
            size_out ? pages_of(size_out) : 0, frame_pages, missing, heap_.describe());
        release_buffers();
        fail(CELL_SPURS_JOB_ERROR_MEMORY_SIZE, job.ea);
        return false;
    }
    if (in_page && size_dma_list) {
        mfc(PS_MFC_GETLF, in_page * kPage, u64{ea_high_input} << 32 | (desc + 0x30), size_dma_list, tag, "job input list");
    }

    // The checks libsre makes before the call: the reserved descriptor fields and the jobbin2 magic (job manager
    // revision > 0), then the binary's marker.
    const u32 binary = binary_page * kPage;
    const u32 magic = ps_read32(ls_, binary + 0x20);
    const bool jobbin2 = magic == 0x42494E32 || magic == 0x62696E32; // "BIN2", "bin2"
    if (ps_read8(base_, jc_ + jc_off::jmVer) &&
        (ps_read32(ls_, desc + 0x28) || (ps_read32(ls_, desc + 0x2C) & 0xFFFFFF) || jobbin2 != ((job_type & 4) != 0) ||
            (jobbin2 && ps_read32(ls_, desc + 0x00) != ps_read16(ls_, binary + 0x2E)))) {
        release_buffers();
        fail(CELL_SPURS_JOB_ERROR_JOB_DESCRIPTOR, job.ea);
        return false;
    }
    const u32 marker = ps_read32(ls_, binary + 0x10);
    if (marker != kBinaryMarkerA && marker != kBinaryMarkerB) {
        release_buffers();
        fail(CELL_SPURS_JOB_ERROR_PERM, job.ea);
        return false;
    }
    if (ea_word & 2) {
        PS_LOG_WARN(kJobChan, "SPURS {:#x} SPU {}: job {:#x} asks for a debugger break before it runs (ignored)",
            w_.sched.spurs, w_.spu_num, job.ea);
    }

    // The job's context (CellSpursJobContext2).
    const u32 c = ls::jc_context;
    std::memset(ls_ + c, 0, 0x30);
    ps_write32(ls_, c + 0x00, in_page * kPage);
    for (u32 k = 0; k < kMaxCache; k++) ps_write32(ls_, c + 0x04 + 4 * k, cache_lsa[k]);
    ps_write8(ls_, c + 0x14, static_cast<u8>((job.size / 128) << 4));
    ps_write16(ls_, c + 0x18, static_cast<u16>(size_dma_list / 8));
    ps_write16(ls_, c + 0x1A, static_cast<u16>(num_cache));
    ps_write32(ls_, c + 0x1C, out_page * kPage);
    ps_write32(ls_, c + 0x20, stack_page_ * kPage);
    ps_write32(ls_, c + 0x24, tag);
    ps_write64(ls_, c + 0x28, job.ea);

    // Registers (D-004: u32[3] is the preferred word). The stack: $sp 0x30 below the top of scratch + stack, word 1
    // the stack's size (less 0x30 from SDK 2.1), word 2 the allocation's size, and a terminating frame.
    SPUContext& ctx = w_.ctx;
    for (auto& r : ctx.gpr) r = ps_spu_v128{};
    const u32 frame = stack_pages_ * kPage;
    const u32 sp = stack_page_ * kPage + frame - 0x30;
    const u32 avail = sdk_ > 0x20FFFF ? size_stack - 0x30 : size_stack;
    ctx.gpr[1].u32[3] = sp;
    ctx.gpr[1].u32[2] = avail;
    ctx.gpr[1].u32[1] = frame;
    std::memset(ls_ + sp, 0, 0x30);
    ps_write32(ls_, sp + 0x0, sp + 0x20);
    ps_write32(ls_, sp + 0x4, avail + 0x20);
    ps_write32(ls_, sp + 0x8, frame + 0x20);
    ps_write32(ls_, sp + 0xC, 0x20);
    ctx.gpr[0].u32[3] = ls::jc_return;
    ctx.gpr[3].u32[3] = c;
    ctx.gpr[4].u32[3] = desc;
    ctx.gpr[80].u32[3] = kSavedSp;
    ctx.gpr[81].u32[3] = kSavedLr;
    ctx.gpr[82].u32[3] = kSaved126;
    ctx.gpr[126].u32[3] = binary;
    ps_spu_wrch(&ctx, ls::jc_return, PS_MFC_WrTagMask, 1u << tag);
    ctx.pc = binary;
    // The binary's lifted program (D-076): identified when the binary came into the LS cache, kept while it stays.
    const SpuImage image{ImageKind::Job, binary_ea, binary, size_binary, binary};
    CacheEntry* cached = binary_entry >= 0 ? &cache_[static_cast<u32>(binary_entry)] : nullptr;
    if (cached && !cached->identified) {
        cached->program = identify_program(base_, image);
        cached->identified = true;
    }
    ps_spu_attach_program_at(&ctx, cached ? cached->program : identify_program(base_, image), binary); // relocatable (D-081)

    ps_spu_set_dma_tag((u64{binary_ea} << 32) | job.ea); // the job's DMA writes carry it (D-138)
    for (;;) {
        const u32 reason = ps_spu_run(&ctx);
        if (reason == PS_SPU_STOP_SIGNAL && ctx.stop_code == kJobReturnStop) break;
        ps_spu_set_dma_tag(policy_tag());
        if (reason == PS_SPU_STOP_REQUESTED) return false;
        fatal(std::format("job {:#x} (binary {:#x} at LS {:#x}) stopped (reason {}, code {:#x}{}, exception {}) at 0x{:05x}",
            job.ea, binary_ea, binary, reason, ctx.stop_code, ctx.stop_code == 0x3FFF ? ", stopd: an assertion" : "",
            ctx.exception, ctx.pc));
    }
    ps_spu_set_dma_tag(policy_tag());
    // A DMA over the binary's code dropped its lifted program: the cached copy is interpreted from now on.
    if (cached && !ctx.home) cached->program = nullptr;
    // The job's own DMA (its output) completed with it: the MFC is synchronous.
    release_buffers();
    return true;
}

} // namespace

void run_jobchain(Worker& w, u32 wid, u32 job_chain)
{
    JobChainPolicy(w, wid, job_chain).run();
}

} // namespace pspocket::hle::spurs

namespace pspocket::hle {

u64 job_descriptors_changed() { return spurs::g_descriptors_changed.load(std::memory_order_relaxed); }

} // namespace pspocket::hle
