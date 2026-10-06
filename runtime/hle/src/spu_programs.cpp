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

#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
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

std::atomic<u64> g_attached{0}, g_unknown{0}, g_images{0}, g_modules{0}, g_modified{0};
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
        out.write(reinterpret_cast<const char*>(base + image.guest), image.size);
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
// the RSX's texture checks), is the program found then. Only lifted programs are kept: an image without one goes the
// whole way each time, as before. Per worker thread, so no lock.
struct KnownImage {
    u32 guest = 0, size = 0, ls_start = 0;
    u64 check = 0, hash = 0;
    u64 registry = 0; // ps_spu_registry_generation when it was found: a program unregistered since is not used
    const ps_spu_program* program = nullptr;
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
    const u8* bytes = base + image.guest;
    const u64 check = content_check(bytes, image.size);
    const u64 registry = ps_spu_registry_generation();
    KnownImage& k = t_known[((image.guest >> 4) ^ (image.size >> 2) ^ image.ls_start) % kKnownImages];
    if (k.program && k.registry == registry && k.guest == image.guest && k.size == image.size &&
        k.ls_start == image.ls_start && k.check == check) {
        g_attached.fetch_add(1, std::memory_order_relaxed);
        if (hash) *hash = k.hash;
        return k.program;
    }
    const u64 h = ps_spu_hash64(bytes, image.size);
    const ps_spu_program* program = identify_hashed(h, base, image);
    if (program) k = {image.guest, image.size, image.ls_start, check, h, registry, program};
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
// copy (patched after loading) is not attached: its lifted code could be stale. Per SPU thread, so no lock.

// The runs live until a GET overwrites part of them: GTA V's dependency tasks copy hundreds of small pieces of data
// between loading a module and running it (run 068: with 128 runs, the oldest that had not run yet, the module, was
// replaced). Each 1 KB block of local store lists the runs that touch it, so a GET only looks at the runs it may
// overwrite, and the resolver only at the runs in pc's block.
constexpr u32 kBlockShift = 10;
constexpr u32 kBlocks = PS_SPU_LS_SIZE >> kBlockShift;
constexpr u32 kRuns = 4096;        // live runs per SPU, more than its 256 KB holds in practice
constexpr u32 kRunsPerBlock = 32;  // a block of smaller pieces drops its oldest

// A run is what one contiguous copy left in local store. A later GET over part of it leaves the rest (run 109:
// Assassin's Creed's tasks load a 42 KB module at 0x4c00, then data over its last 10 KB, and run the module's head):
// the run keeps the extent it was copied with as its identity, and the part still intact as where it holds code.
struct Run {
    u32 ls_start = 0, ls_end = 0; // [ls_start, ls_end): the part of the copy still in local store; empty: a free slot
    u32 id_start = 0, id_end = 0; // [id_start, id_end): the whole copy, as it came (its identity)
    u32 ea_start = 0;             // where the copy's first byte (at id_start) came from
    bool resolved = false;        // identified (the program may still be null)
    u64 hash = 0;
    const ps_spu_program* program = nullptr;

    bool live() const { return ls_end > ls_start; }
    bool holds(u32 pc) const { return pc >= ls_start && pc < ls_end; }
};

Run make_run(u32 ls_start, u32 ls_end, u32 ea_start, u32 id_start, u32 id_end)
{
    Run r;
    r.ls_start = ls_start;
    r.ls_end = ls_end;
    r.id_start = id_start;
    r.id_end = id_end;
    r.ea_start = ea_start;
    return r;
}

struct Block {
    u32 count = 0;
    std::array<u16, kRunsPerBlock> runs{}; // indices of the runs touching the block
};

struct Modules {
    const SPUContext* ctx = nullptr; // the context these runs belong to (one SPU per thread)
    std::array<Block, kBlocks> blocks{};
    std::array<Run, kRuns> runs{};
    u32 next = 0;
    u32 last = kRuns; // the run the last GET started or extended
};

thread_local Modules t_modules;

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

// Forgets every run: the live ones are found through the blocks (resetting all 4096 would cost more than a task start).
void reset(Modules& m)
{
    for (Block& b : m.blocks) {
        for (u32 k = 0; k < b.count; k++) m.runs[b.runs[k]] = Run{};
        b.count = 0;
    }
    m.next = 0;
    m.last = kRuns;
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

void detach_blocks(Modules& m, u32 index)
{
    const Run& r = m.runs[index];
    for (u32 b = r.ls_start >> kBlockShift; b <= (r.ls_end - 1) >> kBlockShift; b++) {
        Block& block = m.blocks[b];
        for (u32 k = 0; k < block.count; k++) {
            if (block.runs[k] != index) continue;
            block.runs[k] = block.runs[--block.count];
            break;
        }
    }
}

void drop(Modules& m, u32 index)
{
    detach_blocks(m, index);
    m.runs[index] = Run{};
    if (m.last == index) m.last = kRuns;
}

void attach_blocks(Modules& m, u32 index)
{
    const Run& r = m.runs[index];
    for (u32 b = r.ls_start >> kBlockShift; b <= (r.ls_end - 1) >> kBlockShift; b++) {
        Block& block = m.blocks[b];
        if (block.count == kRunsPerBlock) drop(m, block.runs[0]);
        block.runs[block.count++] = static_cast<u16>(index);
    }
}

// The slot for a new run: the next free one, else (every slot live) the next one.
u32 take_slot(Modules& m)
{
    for (u32 k = 0; k < kRuns; k++) {
        const u32 i = (m.next + k) % kRuns;
        if (!m.runs[i].live()) {
            m.next = (i + 1) % kRuns;
            return i;
        }
    }
    const u32 i = m.next;
    drop(m, i);
    m.next = (m.next + 1) % kRuns;
    return i;
}

void observe_get(SPUContext* ctx, uint32_t lsa, uint32_t eal, uint32_t size)
{
    if (!size) return;
    Modules& m = modules_for(ctx);
    lsa &= PS_SPU_LS_MASK;
    const u32 end = std::min<u32>(lsa + size, PS_SPU_LS_SIZE);
    const bool extends = m.last < kRuns && m.runs[m.last].live() && m.runs[m.last].ls_end == lsa &&
                         m.runs[m.last].id_end == lsa &&
                         m.runs[m.last].ea_start + (m.runs[m.last].id_end - m.runs[m.last].id_start) == eal;
    note_event('g', lsa, end, eal);
    // Runs this GET overwrites keep what it leaves (not the one it extends: that one ends where this one starts):
    // the part before it, else the part after it; one it covers entirely is gone.
    std::array<u32, 64> overlapping{};
    u32 count = 0;
    for (u32 b = lsa >> kBlockShift; b <= (end - 1) >> kBlockShift; b++) {
        const Block& block = m.blocks[b];
        for (u32 k = 0; k < block.count; k++) {
            const u32 index = block.runs[k];
            const Run& r = m.runs[index];
            if (!(r.ls_start < end && lsa < r.ls_end)) continue;
            if (std::find(overlapping.begin(), overlapping.begin() + count, index) != overlapping.begin() + count) continue;
            if (count < overlapping.size()) overlapping[count++] = index;
        }
    }
    for (u32 i = 0; i < count; i++) {
        const u32 index = overlapping[i];
        Run& r = m.runs[index];
        if (!r.live()) continue; // dropped meanwhile by a full block
        note_event('c', r.ls_start, r.ls_end, r.ea_start);
        const u32 left_end = std::min(lsa, r.ls_end), right_start = std::max(end, r.ls_start);
        if (left_end <= r.ls_start && right_start >= r.ls_end) {
            drop(m, index);
            continue;
        }
        detach_blocks(m, index);
        if (left_end > r.ls_start) r.ls_end = left_end;
        else r.ls_start = right_start;
        attach_blocks(m, index);
        if (m.last == index) m.last = kRuns;
        // Identified again when it runs next: what the copy wrote over may be the program's code.
        r.resolved = false;
        r.hash = 0;
        r.program = nullptr;
    }
    u32 index = m.last;
    if (extends && index < kRuns) {
        detach_blocks(m, index);
        Run& r = m.runs[index];
        r.ls_end = end;
        r.id_end = end;
        r.resolved = false;
        r.hash = 0;
        r.program = nullptr;
    } else {
        index = take_slot(m);
        m.runs[index] = make_run(lsa, end, eal, lsa, end);
    }
    m.last = index;
    attach_blocks(m, index);
}

// A module whose local store copy is not its guest copy any more: logged once per guest address, with where the copies
// part (what the program changes in it before it runs).
void log_modified(const u8* ls, const u8* guest, u32 size, u32 ls_start, u32 ea_start)
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
        "copy, +{:#x} to +{:#x}; interpreted", ls_start, size, ea_start, words, first, last);
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
        nullptr},
    {"spurs_stop_return", 0x5350555253000002ull, kKernelReturn, 4, PS_SPU_XFLOAT_APPROXIMATE, 0, 1, kReturnEntry, nullptr,
        nullptr},
    {"spurs_stop_exit", 0x5350555253000003ull, kKernelExit, 4, PS_SPU_XFLOAT_APPROXIMATE, 0, 1, kExitEntry, nullptr,
        nullptr},
    {"spurs_stop_job_return", 0x5350555253000004ull, ls::jc_return, 4, PS_SPU_XFLOAT_APPROXIMATE, 0, 1, kJobReturnEntry,
        nullptr, nullptr},
    {"spurs_stop_syscall", 0x5350555253000005ull, ls::pm_syscall, 4, PS_SPU_XFLOAT_APPROXIMATE, 0, 1, kSyscallEntry,
        nullptr, nullptr},
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
    Run* found = nullptr;
    // Runs share a block when one ends inside it and another starts there: GTA V's dependency tasks copy a module to
    // 0x71f0 and its data to 0x7310 (run 066: the data's run took the block, and the modules were interpreted).
    const Block& block = m.blocks[pc >> kBlockShift];
    for (u32 k = 0; k < block.count && !found; k++)
        if (m.runs[block.runs[k]].holds(pc)) found = &m.runs[block.runs[k]];
    if (!found) {
        trace_no_run(ctx, pc);
        return nullptr;
    }
    Run& r = *found;
    const u32 size = (r.id_end - r.id_start) & ~3u;
    if (!r.resolved) {
        r.resolved = true;
        u8* base = ctx->membase;
        // The part still in local store must be its guest copy; the whole copy is the module's identity.
        const u32 held = r.ls_end - r.ls_start, from = r.ea_start + (r.ls_start - r.id_start);
        if (std::memcmp(ctx->ls + r.ls_start, base + from, held) != 0) {
            g_modified.fetch_add(1, std::memory_order_relaxed);
            log_modified(ctx->ls + r.ls_start, base + from, held, r.ls_start, from);
            return nullptr;
        }
        r.program = identify(base, {ImageKind::Module, r.ea_start, r.id_start, size, pc}, &r.hash);
        // A copy over part of it may only have written over its data (D-085): code there would be stale.
        if (r.program && lifted_outside(r.program, r.ls_start - r.id_start, r.ls_end - r.id_start)) {
            g_modified.fetch_add(1, std::memory_order_relaxed);
            log_cut_code(r);
            r.program = nullptr;
            return nullptr;
        }
        if (r.program) g_modules.fetch_add(1, std::memory_order_relaxed);
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
    for (u32 b = 0; b < kBlocks; b++) {
        const Block& block = m.blocks[b];
        for (u32 k = 0; k < block.count; k++) {
            const Run& r = m.runs[block.runs[k]];
            // Each run once, in the block it starts in.
            if ((r.ls_start >> kBlockShift) == b && keep(r.ls_start, r.ls_end))
                runs.push_back({r.ls_start, r.ls_end, r.ea_start, r.id_start, r.id_end});
        }
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
        const u32 index = take_slot(m);
        m.runs[index] = make_run(saved.ls_start, saved.ls_end, saved.ea_start, saved.id_start, saved.id_end);
        attach_blocks(m, index);
    }
    m.last = kRuns;
}

void forget_module_runs(SPUContext& ctx)
{
    note_event('f', 0, 0, 0);
    reset(modules_for(&ctx));
}

const ps_spu_program* identify_program(u8* base, const SpuImage& image) { return identify(base, image, nullptr); }

void attach_program(SPUContext& ctx, u8* base, const SpuImage& image)
{
    ps_spu_attach_program_at(&ctx, identify_program(base, image), image.ls_start);
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
        spurs::g_modified.load(std::memory_order_relaxed)};
}

} // namespace pspocket::hle
