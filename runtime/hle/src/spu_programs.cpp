// SPU programs as SPURS loads them (spu_programs.hpp, D-076): the lifted program for each image, the dump of the
// images that have none, and (D-079) the modules programs load themselves.

#include "spu_programs.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/spu/registry.hpp"
#include "pspocket/spu/runtime.h"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/recomp/self_decrypt.hpp"
#include "spurs_native.hpp"

namespace pspocket::hle::spurs {
namespace {

constexpr std::string_view kChan = "spurs";

const char* kind_name(ImageKind k)
{
    switch (k) {
    case ImageKind::Task: return "task";
    case ImageKind::Job: return "job";
    case ImageKind::PolicyModule: return "policy-module";
    case ImageKind::Module: return "module";
    case ImageKind::Thread: return "thread";
    }
    return "?";
}

// Job binaries and the modules programs load are position independent: their lifted programs run at any LS address
// (D-081), so they are known by hash alone.
bool relocatable(ImageKind k) { return k == ImageKind::Job || k == ImageKind::Module; }

struct Registry {
    std::mutex mutex;
    std::string dump_dir;
    std::unordered_set<u64> seen;           // images without a lifted program: hash ^ LS address, or hash (relocatable)
    std::unordered_map<u64, u32> dump_base; // relocatable images saved by this run: hash -> the LS address they name
};

Registry& registry()
{
    static Registry r;
    return r;
}

std::atomic<u64> g_attached{0}, g_unknown{0}, g_images{0}, g_modules{0}, g_modified{0}, g_found_again{0}, g_none_again{0},
    g_reidentified{0};
std::atomic<bool> g_dumping{false};

// The file name of an image: its program key, as spu-lift names the program (see scripts/lift-spu.sh).
std::string image_name(u64 hash, u32 ls_start) { return std::format("spu_{:016x}_{:05x}", hash, ls_start); }

void dump(const std::string& dir, u8* base, const SpuImage& image, u64 hash)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string name = image_name(hash, image.ls_start);
    const std::filesystem::path bin = std::filesystem::path(dir) / (name + ".bin");
    if (std::filesystem::exists(bin, ec)) return; // an earlier run saved it
    {
        std::ofstream out(bin, std::ios::binary);
        out.write(reinterpret_cast<const char*>(image.bytes ? image.bytes : base + image.guest), image.size);
        if (!out) {
            PS_LOG_ERROR(kChan, "--spu-dump: cannot write {}", bin.string());
            return;
        }
    }
    std::ofstream meta(std::filesystem::path(dir) / (name + ".txt"));
    meta << std::format("kind {}\nguest {:#x}\nbase {:#x}\nsize {:#x}\nentry {:#x}\n", kind_name(image.kind), image.guest,
        image.ls_start, image.size, image.entry);
}

// Another entry of a module (at pc, its code at ls_start): a seed for its next lift (scripts/lift-spu.sh reads "seed"
// lines), where the saved image has it. Once per thread and offset, then appended under the registry's lock.
thread_local std::unordered_set<u64> t_seeds;

void dump_seed(u64 hash, u32 ls_start, u32 pc)
{
    const u32 offset = (pc - ls_start) & PS_SPU_LS_MASK;
    if (!g_dumping.load(std::memory_order_relaxed) || !t_seeds.insert((hash << 18) ^ offset).second) return;
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    const auto it = r.dump_base.find(hash);
    if (r.dump_dir.empty() || it == r.dump_base.end()) return;
    std::ofstream meta(std::filesystem::path(r.dump_dir) / (image_name(hash, it->second) + ".txt"), std::ios::app);
    meta << std::format("seed {:#x}\n", (it->second + offset) & PS_SPU_LS_MASK);
}

const ps_spu_program* identify_hashed(u64 hash, u8* base, const SpuImage& image);

// ---- identifications again (D-109)
//
// Jobs and modules load from the same guest addresses thousands of times a second, and each load hashed its whole
// image and searched the registry under its lock (3.6% of a core in run 080's profile). An image found before, at the
// same guest address, size and local store address, with the same bytes by a hash four times as fast (four lanes, as
// the RSX's texture checks), is the program found then. So is an image without one (D-232): Assassin's Creed's first
// session loads its jobs and modules untranslated 180,000 times in 150 s (run 207), each hashed whole and looked up
// under the registry's lock. Per worker thread, so no lock.
struct KnownImage {
    u32 guest = 0, size = 0, ls_start = 0;
    u64 check = 0, hash = 0;
    // The registry's counts when it was looked up: a program found holds until one is unregistered, "none" until one
    // is registered (D-231).
    u64 removals = 0, additions = 0;
    const ps_spu_program* program = nullptr;
    bool filled = false;
};
constexpr u32 kKnownImages = 256;
thread_local std::array<KnownImage, kKnownImages> t_known{};

u64 content_check(const u8* data, usize size)
{
    constexpr u64 kPrime = 0x100000001b3ull;
    u64 h0 = 0xcbf29ce484222325ull, h1 = 0x9e3779b97f4a7c15ull, h2 = 0xc2b2ae3d27d4eb4full, h3 = 0x165667b19e3779f9ull;
    usize i = 0;
    for (; i + 32 <= size; i += 32) {
        u64 w[4];
        std::memcpy(w, data + i, 32);
        h0 = (h0 ^ w[0]) * kPrime;
        h1 = (h1 ^ w[1]) * kPrime;
        h2 = (h2 ^ w[2]) * kPrime;
        h3 = (h3 ^ w[3]) * kPrime;
    }
    u64 h = h0 ^ std::rotl(h1, 17) ^ std::rotl(h2, 31) ^ std::rotl(h3, 47);
    for (; i < size; i++) h = (h ^ data[i]) * kPrime;
    return h ^ size;
}

// identify_hashed, by way of the images found before; *hash gets the image's program hash.
const ps_spu_program* identify(u8* base, const SpuImage& image, u64* hash)
{
    const u8* bytes = image.bytes ? image.bytes : base + image.guest;
    const u64 check = content_check(bytes, image.size);
    // Read before the lookup: a program registered meanwhile makes the entry stale, never wrong.
    const u64 additions = spu::registry_additions(), removals = spu::registry_removals();
    KnownImage& k = t_known[((image.guest >> 4) ^ (image.size >> 2) ^ image.ls_start) % kKnownImages];
    if (k.filled && k.guest == image.guest && k.size == image.size && k.ls_start == image.ls_start && k.check == check) {
        if (k.program && k.removals == removals) {
            g_attached.fetch_add(1, std::memory_order_relaxed);
            g_found_again.fetch_add(1, std::memory_order_relaxed);
            if (hash) *hash = k.hash;
            return k.program;
        }
        if (!k.program && k.additions == additions) { // seen (and saved) the first time
            g_unknown.fetch_add(1, std::memory_order_relaxed);
            g_none_again.fetch_add(1, std::memory_order_relaxed);
            if (hash) *hash = k.hash;
            return nullptr;
        }
    }
    const u64 h = ps_spu_hash64(bytes, image.size);
    const ps_spu_program* program = identify_hashed(h, base, image);
    k = {image.guest, image.size, image.ls_start, check, h, removals, additions, program, true};
    if (hash) *hash = h;
    return program;
}

// ---- modules (D-079)
//
// A program can copy code into local store itself and run it: GTA V's dependency tasks (taskset "dependency_spu")
// load the jobs they run as code modules. SPURS never sees those loads, so the run loop asks (ps_spu_set_resolver)
// when execution reaches an address no attached program has lifted code for. Each SPU's GETs into local store are kept
// as runs of contiguous copies (one module comes in several 16 KB GETs); the run holding pc is the module, known by the
// hash of its guest copy and its LS address like any image. A module whose local store copy differs from its guest
// copy is known by its local store copy instead (D-206): a program lifted from other bytes would be stale. Per SPU
// thread, so no lock.

// The runs live until GETs overwrite them: GTA V's dependency tasks copy hundreds of small pieces of data between
// loading a module and running it (run 068: with 128 runs, the oldest that had not run yet, the module, was replaced).
//
// Each 16-byte line of local store records the run that wrote it last (D-220): a GET takes the lines it covers from
// the runs that held them, which are identified again when they run next; the run holding pc is its line's, and the
// part of that run still in local store is the lines around pc it still holds. Before, each 1 KB block listed the
// runs touching it and a GET tested about 12 runs and cut 1.4 of them: 240 ns a GET, 11% of Assassin's Creed's busy
// SPU time (run 126). A later GET over part of a run leaves the rest (run 109: Assassin's Creed's tasks load a 42 KB
// module at 0x4c00, then data over its last 10 KB, and run the module's head): the run keeps the extent it was copied
// with as its identity.
//
// A run lives while it holds a line: a module stays known however many copies go elsewhere after it (it is looked up
// again each time execution comes back to it from the task's own code). A line names its run by slot and epoch, so
// forgetting everything (a task switch) is one increment.
constexpr u32 kLineShift = 4;
constexpr u32 kLines = PS_SPU_LS_SIZE >> kLineShift;
constexpr u32 kSlotBits = 12;
constexpr u32 kSlots = 1u << kSlotBits; // live runs per SPU, more than its 256 KB holds in practice
constexpr u32 kSlotMask = kSlots - 1;
constexpr u32 kEpochs = 1u << (32 - kSlotBits);
constexpr u32 kNoRun = ~0u;

struct Run {
    u32 lines = 0;                // the lines of local store it holds (0: a free slot)
    u32 id_start = 0, id_end = 0; // [id_start, id_end): the whole copy, as it came (its identity)
    u32 ea_start = 0;             // where the copy's first byte (at id_start) came from
    u32 ls_start = 0, ls_end = 0; // the part of it still in local store when it was identified
    bool resolved = false;        // identified (the program may still be null) for [ls_start, ls_end)
    u64 hash = 0;
    const ps_spu_program* program = nullptr;
    u64 additions = 0; // spu::registry_additions() when it was identified: with none, looked up again after (D-234)
};

struct Modules {
    const SPUContext* ctx = nullptr;   // the context these runs belong to (one SPU per thread)
    std::array<u32, kLines> owner{};   // epoch << kSlotBits | slot of the run that wrote each line last
    std::array<Run, kSlots> runs{};
    std::array<u16, kSlots> free_slots{}; // slots freed in this epoch
    u32 free_count = 0;
    u32 fresh = 0;      // slots from here on are unused in this epoch
    u32 evict = 0;      // the next slot to give up when every one is live
    u32 epoch = 1;      // a line of another epoch holds no run
    u32 last = kNoRun;  // the slot of the run the last GET started or extended
};

thread_local Modules t_modules;

u32 tag_of(const Modules& m, u32 slot) { return m.epoch << kSlotBits | slot; }

Run* run_at(Modules& m, u32 tag) { return (tag >> kSlotBits) == m.epoch ? &m.runs[tag & kSlotMask] : nullptr; }

// Lines [first, last] lose the runs that held them (except the run tagged keep): each is identified again when it runs
// next, since what is written there may be its code, and one left with no line is gone.
void release(Modules& m, u32 first, u32 last, u32 keep);

// A slot for a new run: a freed one, else one not used yet, else (every one live) the next one given up.
u32 take_slot(Modules& m)
{
    if (m.free_count) return m.free_slots[--m.free_count];
    if (m.fresh < kSlots) return m.fresh++;
    const u32 slot = m.evict;
    m.evict = (m.evict + 1) & kSlotMask;
    const Run& r = m.runs[slot];
    const u32 tag = tag_of(m, slot);
    for (u32 l = r.id_start >> kLineShift; l <= (r.id_end - 1) >> kLineShift; l++)
        if (m.owner[l] == tag) m.owner[l] = 0;
    if (m.last == slot) m.last = kNoRun;
    return slot;
}

// A new run that holds [ls_start, ls_end) of the copy [id_start, id_end) from ea_start; returns its slot.
u32 new_run(Modules& m, u32 ls_start, u32 ls_end, u32 ea_start, u32 id_start, u32 id_end)
{
    const u32 first = ls_start >> kLineShift, last = (ls_end - 1) >> kLineShift;
    release(m, first, last, 0);
    const u32 slot = take_slot(m);
    Run& r = m.runs[slot];
    r = Run{};
    r.id_start = id_start;
    r.id_end = id_end;
    r.ea_start = ea_start;
    r.lines = last - first + 1;
    std::fill(m.owner.begin() + first, m.owner.begin() + last + 1, tag_of(m, slot));
    return slot;
}

// PSPOCKET_SPU_TRACE_RUNS=N (debugging): when the resolver finds no run at an address, print the tracker's last events
// over that address's 1 KB block, for the first N such addresses.
struct RunEvent {
    char kind; // g: GET, c: run cleared by a GET, f: forget, r: restore
    u32 ls_start, ls_end, ea;
};
thread_local std::array<RunEvent, 256> t_events{};
thread_local u32 t_event_next = 0;
std::atomic<int> g_trace_runs{-1};

bool tracing_runs()
{
    int n = g_trace_runs.load(std::memory_order_relaxed);
    if (n < 0) {
        const char* v = std::getenv("PSPOCKET_SPU_TRACE_RUNS");
        n = v ? std::atoi(v) : 0;
        g_trace_runs.store(n, std::memory_order_relaxed);
    }
    return n > 0;
}

void note_event(char kind, u32 ls_start, u32 ls_end, u32 ea)
{
    if (!tracing_runs()) return;
    t_events[t_event_next++ % t_events.size()] = RunEvent{kind, ls_start, ls_end, ea};
}

void trace_no_run(const SPUContext* ctx, u32 pc)
{
    if (!tracing_runs() || g_trace_runs.fetch_sub(1, std::memory_order_relaxed) <= 0) return;
    std::string out = std::format("SPU {}: no run at 0x{:05x} (program {}); the last events over its block:", ctx->id, pc,
        ctx->program ? ctx->program->name : "none");
    const u32 block = pc >> 10;
    for (u32 k = 0; k < t_events.size(); k++) {
        const RunEvent& e = t_events[(t_event_next + k) % t_events.size()];
        if (!e.kind) continue;
        if (e.kind != 'f' && (e.ls_end <= block << 10 || e.ls_start >= (block + 1) << 10)) continue;
        out += std::format(" [{} 0x{:05x}-0x{:05x} ea {:#x}]", e.kind, e.ls_start, e.ls_end, e.ea);
    }
    PS_LOG_INFO(kChan, "{}", out);
}

// The first line in [from, end) that `tag` does not own, or end. A 16 KB GET covers 1,024 lines, usually of one run:
// eight at a time (GTA V's 16 KB module loads made release 3% of the SPU threads' busy time, run 157).
u32 run_end(const u32* owner, u32 from, u32 end, u32 tag)
{
#if defined(__aarch64__)
    const uint32x4_t t = vdupq_n_u32(tag);
    while (from + 8 <= end) {
        const uint32x4_t same = vandq_u32(vceqq_u32(vld1q_u32(owner + from), t), vceqq_u32(vld1q_u32(owner + from + 4), t));
        if (vminvq_u32(same) != ~0u) break;
        from += 8;
    }
#endif
    while (from < end && owner[from] == tag) from++;
    return from;
}

void release(Modules& m, u32 first, u32 last, u32 keep)
{
    for (u32 l = first; l <= last;) {
        const u32 tag = m.owner[l];
        const u32 e = run_end(m.owner.data(), l + 1, last + 1, tag);
        if (Run* r = tag != keep ? run_at(m, tag) : nullptr) {
            note_event('c', r->id_start, r->id_end, r->ea_start);
            r->resolved = false;
            r->lines -= e - l;
            if (!r->lines) m.free_slots[m.free_count++] = static_cast<u16>(tag & kSlotMask);
        }
        l = e;
    }
}

// Forgets every run.
void reset(Modules& m)
{
    if (++m.epoch == kEpochs) { // once in a million resets: no line may keep a tag of the epoch that comes back
        m.owner.fill(0);
        m.epoch = 1;
    }
    m.free_count = m.fresh = m.evict = 0;
    m.last = kNoRun;
}

Modules& modules_for(const SPUContext* ctx)
{
    Modules& m = t_modules;
    if (m.ctx != ctx) {
        reset(m);
        m.ctx = ctx;
    }
    return m;
}

void observe_get(SPUContext* ctx, uint32_t lsa, uint32_t eal, uint32_t size)
{
    if (!size) return;
    Modules& m = modules_for(ctx);
    lsa &= PS_SPU_LS_MASK;
    const u32 end = std::min<u32>(lsa + size, PS_SPU_LS_SIZE);
    note_event('g', lsa, end, eal);
    // The copy goes on where the last GET's ended, in local store and in guest memory: the same run, which keeps the
    // line it may share with this GET.
    if (m.last != kNoRun) {
        Run& prev = m.runs[m.last];
        if (prev.id_end == lsa && prev.ea_start + (prev.id_end - prev.id_start) == eal) {
            const u32 tag = tag_of(m, m.last);
            u32 first = lsa >> kLineShift;
            const u32 last = (end - 1) >> kLineShift;
            release(m, first, last, tag);
            if (m.owner[first] == tag) first++;
            prev.id_end = end;
            prev.resolved = false;
            prev.lines += last + 1 - first;
            std::fill(m.owner.begin() + first, m.owner.begin() + last + 1, tag);
            return;
        }
    }
    m.last = new_run(m, lsa, end, eal, lsa, end);
}

// A module whose local store copy is not its guest copy any more: logged once per guest address, with where the copies
// part (what the program changes in it before it runs).
void log_modified(const u8* ls, const u8* guest, u32 size, u32 ls_start, u32 ea_start, bool whole)
{
    static std::mutex mutex;
    static std::unordered_set<u32> logged;
    {
        std::lock_guard lock(mutex);
        if (!logged.insert(ea_start).second) return;
    }
    u32 first = 0, last = 0, words = 0;
    for (u32 i = 0; i + 4 <= size; i += 4)
        if (std::memcmp(ls + i, guest + i, 4) != 0) {
            if (!words++) first = i;
            last = i;
        }
    PS_LOG_INFO(kChan, "SPU module at LS 0x{:05x}+{:#x} (guest {:#x}) changed before it ran: {} words differ from its guest "
        "copy, +{:#x} to +{:#x}; {}", ls_start, size, ea_start, words, first, last,
        whole ? "known by its local store copy" : "interpreted");
}

// Whether program p lifted any instruction outside [from, to) (offsets from its start): what a copy over part of its
// module wrote over (D-085: code_words; none means all words are instructions).
bool lifted_outside(const ps_spu_program* p, u32 from, u32 to)
{
    for (u32 w = 0; w < p->code_size / 4; w++) {
        if (w * 4 >= from && w * 4 < to) continue;
        if (!p->code_words || ((p->code_words[w >> 5] >> (w & 31)) & 1)) return true;
    }
    return false;
}

// A module part of whose code was written over: logged once per guest address.
void log_cut_code(const Run& r)
{
    static std::mutex mutex;
    static std::unordered_set<u32> logged;
    {
        std::lock_guard lock(mutex);
        if (!logged.insert(r.ea_start).second) return;
    }
    PS_LOG_INFO(kChan, "SPU module at LS 0x{:05x}+{:#x} (guest {:#x}): a copy wrote over its code, 0x{:05x} to 0x{:05x} "
        "is left; interpreted", r.id_start, r.id_end - r.id_start, r.ea_start, r.ls_start, r.ls_end);
}

// ---- the worker's stops (D-087)
//
// The SPURS kernel's entry points, the job return and the taskset's syscall entry are `stop`s the worker plants in local
// store (spurs_native.hpp).
// Policy modules and jobs reach them from lifted code all the time (run 058: GTA V's own policy module left through
// 0x808 2.1 million times a second), and each time the run loop missed, logged under a lock and interpreted one
// instruction. A lifted stand-in takes the stop instead, while local store still holds it there.
void stop_select(SPUContext* ctx, uint8_t*) { ps_spu_stop(ctx, kKernelSelect, kKernelSelectStop); }
void stop_return(SPUContext* ctx, uint8_t*) { ps_spu_stop(ctx, kKernelReturn, kKernelExitStop); }
void stop_exit(SPUContext* ctx, uint8_t*) { ps_spu_stop(ctx, kKernelExit, kKernelExitStop); }
void stop_job_return(SPUContext* ctx, uint8_t*) { ps_spu_stop(ctx, ls::jc_return, kJobReturnStop); }
void stop_syscall(SPUContext* ctx, uint8_t*) { ps_spu_stop(ctx, ls::pm_syscall, kSyscallStop); }

// One 4-byte program per stop: a wider one would also hold the policy module's code (from 0xA00), and the run loop
// would not go back to the module's own program from it.
const ps_spu_entry kSelectEntry[] = {{kKernelSelect, &stop_select}};
const ps_spu_entry kReturnEntry[] = {{kKernelReturn, &stop_return}};
const ps_spu_entry kExitEntry[] = {{kKernelExit, &stop_exit}};
const ps_spu_entry kJobReturnEntry[] = {{ls::jc_return, &stop_job_return}};
const ps_spu_entry kSyscallEntry[] = {{ls::pm_syscall, &stop_syscall}};
std::array<ps_spu_program, 5> g_stubs{{
    {"spurs_stop_select", 0x5350555253000001ull, kKernelSelect, 4, PS_SPU_XFLOAT_APPROXIMATE, 0, 1, kSelectEntry, nullptr,
        nullptr, 0},
    {"spurs_stop_return", 0x5350555253000002ull, kKernelReturn, 4, PS_SPU_XFLOAT_APPROXIMATE, 0, 1, kReturnEntry, nullptr,
        nullptr, 0},
    {"spurs_stop_exit", 0x5350555253000003ull, kKernelExit, 4, PS_SPU_XFLOAT_APPROXIMATE, 0, 1, kExitEntry, nullptr,
        nullptr, 0},
    {"spurs_stop_job_return", 0x5350555253000004ull, ls::jc_return, 4, PS_SPU_XFLOAT_APPROXIMATE, 0, 1, kJobReturnEntry,
        nullptr, nullptr, 0},
    {"spurs_stop_syscall", 0x5350555253000005ull, ls::pm_syscall, 4, PS_SPU_XFLOAT_APPROXIMATE, 0, 1, kSyscallEntry,
        nullptr, nullptr, 0},
}};

const ps_spu_program* worker_stop(const SPUContext* ctx, u32 pc)
{
    u32 stop = 0;
    const ps_spu_program* program = nullptr;
    switch (pc) {
    case kKernelSelect: stop = kKernelSelectStop, program = &g_stubs[0]; break;
    case kKernelReturn: stop = kKernelExitStop, program = &g_stubs[1]; break;
    case kKernelExit: stop = kKernelExitStop, program = &g_stubs[2]; break;
    case ls::jc_return: stop = kJobReturnStop, program = &g_stubs[3]; break;
    case ls::pm_syscall: stop = kSyscallStop, program = &g_stubs[4]; break; // a task's syscalls (run 066)
    default: return nullptr;
    }
    return ps_read32(ctx->ls, pc) == stop ? program : nullptr; // `stop` encodes its signal in its low 14 bits
}

// The resolver: the lifted program of the module holding pc, and where it sits (its lifted programs are relocatable).
const ps_spu_program* resolve(SPUContext* ctx, uint32_t pc, uint32_t* code_start)
{
    pc &= PS_SPU_LS_MASK & ~3u;
    if (const ps_spu_program* stub = worker_stop(ctx, pc)) {
        *code_start = stub->code_start;
        return stub;
    }
    Modules& m = modules_for(ctx);
    const u32 line = pc >> kLineShift, tag = m.owner[line];
    Run* found = run_at(m, tag);
    if (!found || pc < found->id_start || pc >= found->id_end) {
        trace_no_run(ctx, pc);
        return nullptr;
    }
    Run& r = *found;
    if (r.resolved && (pc < r.ls_start || pc >= r.ls_end)) r.resolved = false; // another part of it, cut in two
    if (!r.resolved) {
        // The part still in local store: the lines around pc it still holds.
        u32 a = line, b = line;
        while (a > (r.id_start >> kLineShift) && m.owner[a - 1] == tag) a--;
        while (b < ((r.id_end - 1) >> kLineShift) && m.owner[b + 1] == tag) b++;
        r.ls_start = std::max(r.id_start, a << kLineShift);
        r.ls_end = std::min(r.id_end, (b + 1) << kLineShift);
        r.hash = 0;
        r.program = nullptr;
    }
    const u32 size = (r.id_end - r.id_start) & ~3u;
    if (!r.resolved) {
        r.resolved = true;
        u8* base = ctx->membase;
        // The part still in local store must be its guest copy; the whole copy is the module's identity. A whole one
        // whose guest copy changed since (the buffer reused, or the module patched where it runs) is known by what
        // local store holds: that is the code that runs (D-206).
        const u32 held = r.ls_end - r.ls_start, from = r.ea_start + (r.ls_start - r.id_start);
        const u8* bytes = nullptr;
        if (std::memcmp(ctx->ls + r.ls_start, base + from, held) != 0) {
            g_modified.fetch_add(1, std::memory_order_relaxed);
            const bool whole = r.ls_start == r.id_start && r.ls_end == r.id_end;
            log_modified(ctx->ls + r.ls_start, base + from, held, r.ls_start, from, whole);
            if (!whole) return nullptr;
            bytes = ctx->ls + r.ls_start;
        }
        r.additions = spu::registry_additions();
        r.program = identify(base, {ImageKind::Module, r.ea_start, r.id_start, size, pc, bytes}, &r.hash);
        // A copy over part of it may only have written over its data (D-085): code there would be stale.
        if (r.program && lifted_outside(r.program, r.ls_start - r.id_start, r.ls_end - r.id_start)) {
            g_modified.fetch_add(1, std::memory_order_relaxed);
            log_cut_code(r);
            r.program = nullptr;
            return nullptr;
        }
        if (r.program) g_modules.fetch_add(1, std::memory_order_relaxed);
    } else if (!r.program && r.hash && spu::registry_additions() != r.additions) {
        // Registered since it was identified (D-234): its program, if its code words in local store are still that
        // program's code (a copy over any of them since leaves it interpreted). A run with no key (cut, or changed and
        // partial) never finds one.
        r.additions = spu::registry_additions();
        r.program = spu::find_program_checked(r.hash, r.id_start, size, true, ctx->ls);
        if (r.program) {
            g_modules.fetch_add(1, std::memory_order_relaxed);
            g_reidentified.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (r.program) {
        *code_start = r.id_start;
        return r.program;
    }
    if (r.hash) dump_seed(r.hash, r.id_start, pc);
    return nullptr;
}

const ps_spu_program* identify_hashed(u64 hash, u8* base, const SpuImage& image)
{
    const ps_spu_program* program = ps_spu_find_program(hash, image.ls_start, image.size);
    // A relocatable image new as a whole may still be a known program with other data: Assassin's Creed loads one
    // 32 KB module whose last 18 words change every time (398 images in a session, run 117, D-208).
    if (!program && relocatable(image.kind))
        program = ps_spu_find_program_by_code(image.bytes ? image.bytes : base + image.guest, image.size);
    if (program) {
        g_attached.fetch_add(1, std::memory_order_relaxed);
        return program;
    }
    g_unknown.fetch_add(1, std::memory_order_relaxed);
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    const bool anywhere = relocatable(image.kind);
    if (!r.seen.insert(anywhere ? hash : hash ^ image.ls_start).second) return nullptr;
    g_images.fetch_add(1, std::memory_order_relaxed);
    // CLAUDE.md rule 5 asks dev builds to abort on an unknown SPU hash. Until the game's programs are collected
    // (D-076), every build logs it once and interprets.
    PS_LOG_INFO(kChan, "SPU {} {} (guest {:#x}, LS 0x{:05x}+{:#x}, entry 0x{:05x}) has no lifted program: interpreted",
        kind_name(image.kind), image_name(hash, image.ls_start), image.guest, image.ls_start, image.size, image.entry);
    if (!r.dump_dir.empty()) {
        dump(r.dump_dir, base, image, hash);
        // A relocatable image is saved once, where it was first seen: its seeds are kept in those terms.
        if (anywhere) r.dump_base.emplace(hash, image.ls_start);
    }
    return nullptr;
}

struct SavedRun {
    u32 ls_start, ls_end, ea_start, id_start, id_end;
};

struct SavedRuns {
    std::mutex mutex;
    std::unordered_map<u32, std::vector<SavedRun>> by_area; // save area -> the runs inside its blocks
};

SavedRuns& saved_runs()
{
    static SavedRuns s;
    return s;
}

} // namespace

void save_module_runs(const SPUContext& ctx, u32 save_area, const std::function<bool(u32, u32)>& keep)
{
    Modules& m = modules_for(&ctx);
    std::vector<SavedRun> runs;
    // Each stretch of lines one run still holds (a run cut in two gives two, with the same identity).
    for (u32 l = 0; l < kLines;) {
        const u32 tag = m.owner[l];
        u32 e = l + 1;
        while (e < kLines && m.owner[e] == tag) e++;
        if (const Run* r = run_at(m, tag)) {
            const u32 ls_start = std::max(r->id_start, l << kLineShift), ls_end = std::min(r->id_end, e << kLineShift);
            if (ls_start < ls_end && keep(ls_start, ls_end))
                runs.push_back({ls_start, ls_end, r->ea_start, r->id_start, r->id_end});
        }
        l = e;
    }
    SavedRuns& s = saved_runs();
    std::lock_guard lock(s.mutex);
    if (runs.empty()) s.by_area.erase(save_area);
    else s.by_area[save_area] = std::move(runs);
}

void restore_module_runs(SPUContext& ctx, u32 save_area)
{
    std::vector<SavedRun> runs;
    {
        SavedRuns& s = saved_runs();
        std::lock_guard lock(s.mutex);
        const auto it = s.by_area.find(save_area);
        if (it == s.by_area.end()) return;
        runs = it->second;
    }
    Modules& m = modules_for(&ctx);
    for (const SavedRun& saved : runs) {
        note_event('r', saved.ls_start, saved.ls_end, saved.ea_start);
        // Identified again when execution reaches it: its guest copy is checked against local store as after a GET.
        new_run(m, saved.ls_start, saved.ls_end, saved.ea_start, saved.id_start, saved.id_end);
    }
    m.last = kNoRun;
}

void forget_module_runs(SPUContext& ctx)
{
    note_event('f', 0, 0, 0);
    reset(modules_for(&ctx));
}

const ps_spu_program* identify_program(u8* base, const SpuImage& image, u64* hash) { return identify(base, image, hash); }

void attach_identified(SPUContext& ctx, const ps_spu_program* program, const SpuImage& image, u64 hash, u64 additions)
{
    ps_spu_attach_program_at(&ctx, program, image.ls_start);
    if (!program) spu::set_pending_home(&ctx, hash, image.ls_start, image.size, relocatable(image.kind), additions);
}

void count_reidentified() { g_reidentified.fetch_add(1, std::memory_order_relaxed); }

void attach_program(SPUContext& ctx, u8* base, const SpuImage& image)
{
    const u64 additions = spu::registry_additions();
    u64 hash = 0;
    const ps_spu_program* program = identify_program(base, image, &hash);
    attach_identified(ctx, program, image, hash, additions);
}

} // namespace pspocket::hle::spurs

namespace pspocket::hle {

// lv2's SPU thread groups (D-167): each thread's image gets its lifted program as SPURS's images do, and an SPU SELF
// opened by sys_spu_image_open decrypts with the build's keys (as the loader's executables do, D-121).
void install_spu_thread_hooks(lv2::Kernel& kernel)
{
    u8* base = kernel.base();
    kernel.set_spu_program_identifier([base](SPUContext& ctx, u32 guest, u32 ls_start, u32 size, u32 entry) {
        spurs::attach_program(ctx, base, {spurs::ImageKind::Thread, guest, ls_start, size, entry});
    });
    kernel.set_self_decrypter([](std::vector<u8>& bytes, std::string& error) {
        auto elf = recomp::decrypt_self(bytes, recomp::builtin_self_keys());
        if (!elf) {
            error = elf.error().message;
            return false;
        }
        bytes = std::move(*elf);
        return true;
    });
}

} // namespace pspocket::hle

namespace pspocket::hle {

void set_spu_dump_dir(std::string dir)
{
    spurs::Registry& r = spurs::registry();
    std::lock_guard lock(r.mutex);
    spurs::g_dumping.store(!dir.empty(), std::memory_order_relaxed);
    r.dump_dir = std::move(dir);
}

void enable_spu_modules()
{
    for (ps_spu_program& stub : spurs::g_stubs) ps_spu_register_program(&stub); // the worker's stops (D-087)
    ps_spu_set_get_observer(&spurs::observe_get);
    ps_spu_set_resolver(&spurs::resolve);
}

SpuProgramStats spu_program_stats()
{
    return {spurs::g_attached.load(std::memory_order_relaxed), spurs::g_unknown.load(std::memory_order_relaxed),
        spurs::g_images.load(std::memory_order_relaxed), spurs::g_modules.load(std::memory_order_relaxed),
        spurs::g_modified.load(std::memory_order_relaxed), spurs::g_found_again.load(std::memory_order_relaxed),
        spurs::g_none_again.load(std::memory_order_relaxed), spurs::g_reidentified.load(std::memory_order_relaxed)};
}

} // namespace pspocket::hle
