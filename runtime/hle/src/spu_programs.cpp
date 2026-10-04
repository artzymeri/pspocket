// SPU programs as SPURS loads them (spu_programs.hpp, D-076): the lifted program for each image, the dump of the
// images that have none, and (D-079) the modules programs load themselves.

#include "spu_programs.hpp"

#include <algorithm>
#include <array>
#include <atomic>
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

// ---- modules (D-079)
//
// A program can copy code into local store itself and run it: GTA V's dependency tasks (taskset "dependency_spu")
// load the jobs they run as code modules. SPURS never sees those loads, so the run loop asks (ps_spu_set_resolver)
// when execution reaches an address no attached program has lifted code for. Each SPU's GETs into local store are kept
// as runs of contiguous copies (one module comes in several 16 KB GETs); the run holding pc is the module, known by the
// hash of its guest copy and its LS address like any image. A module whose local store copy differs from its guest
// copy (patched after loading) is not attached: its lifted code could be stale. Per SPU thread, so no lock.

constexpr u32 kBlockShift = 10; // a run is found through the 1 KB blocks of local store it covers
constexpr u32 kRuns = 128;      // block_run holds index + 1 in a byte

struct Run {
    u32 ls_start = 0, ls_end = 0; // [ls_start, ls_end) of local store
    u32 ea_start = 0;             // where its first byte came from
    bool resolved = false;        // identified (the program may still be null)
    u64 hash = 0;
    const ps_spu_program* program = nullptr;

    bool holds(u32 pc) const { return pc >= ls_start && pc < ls_end; }
};

struct Modules {
    const SPUContext* ctx = nullptr; // the context these runs belong to (one SPU per thread)
    std::array<u8, (PS_SPU_LS_SIZE >> kBlockShift)> block_run{}; // run index + 1 (0: none)
    std::array<Run, kRuns> runs{};
    u32 next = 0;
    u32 last = kRuns; // the run the last GET started or extended
};

thread_local Modules t_modules;

Modules& modules_for(const SPUContext* ctx)
{
    Modules& m = t_modules;
    if (m.ctx != ctx) {
        m = Modules{};
        m.ctx = ctx;
    }
    return m;
}

// The slot for a new run: a free one, else the oldest that never ran (data: most GETs are), else the oldest. A module
// that ran stays known while it is in local store, however much data its program copies around it (run 054: with
// the oldest run always replaced, the dependency tasks' data pushed their modules out, and 4,970 of their addresses
// were interpreted).
u32 take_slot(Modules& m)
{
    for (u32 k = 0; k < kRuns; k++) {
        const u32 i = (m.next + k) % kRuns;
        if (const Run& r = m.runs[i]; r.ls_end == r.ls_start || !r.resolved) {
            m.next = (i + 1) % kRuns;
            return i;
        }
    }
    const u32 i = m.next;
    m.next = (m.next + 1) % kRuns;
    return i;
}

void observe_get(SPUContext* ctx, uint32_t lsa, uint32_t eal, uint32_t size)
{
    if (!size) return;
    Modules& m = modules_for(ctx);
    lsa &= PS_SPU_LS_MASK;
    const u32 end = std::min<u32>(lsa + size, PS_SPU_LS_SIZE);
    const bool extends = m.last < kRuns && m.runs[m.last].ls_end > m.runs[m.last].ls_start &&
                         m.runs[m.last].ls_end == lsa &&
                         m.runs[m.last].ea_start + (m.runs[m.last].ls_end - m.runs[m.last].ls_start) == eal;
    // Runs this GET overwrites lose what they held (not the one it extends: that one ends where this one starts).
    for (Run& r : m.runs) {
        if (r.ls_start < end && lsa < r.ls_end) r = Run{};
    }
    u32 index = m.last;
    if (extends) {
        Run& r = m.runs[index];
        r.ls_end = end;
        r.resolved = false;
        r.hash = 0;
        r.program = nullptr;
    } else {
        index = take_slot(m);
        m.runs[index] = Run{lsa, end, eal};
    }
    m.last = index;
    for (u32 b = lsa >> kBlockShift; b <= (end - 1) >> kBlockShift; b++) m.block_run[b] = static_cast<u8>(index + 1);
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

// ---- the worker's stops (D-087)
//
// The SPURS kernel's entry points and the job return are `stop`s the worker plants in local store (spurs_native.hpp).
// Policy modules and jobs reach them from lifted code all the time (run 058: GTA V's own policy module left through
// 0x808 2.1 million times a second), and each time the run loop missed, logged under a lock and interpreted one
// instruction. A lifted stand-in takes the stop instead, while local store still holds it there.
void stop_select(SPUContext* ctx, uint8_t*) { ps_spu_stop(ctx, kKernelSelect, kKernelSelectStop); }
void stop_return(SPUContext* ctx, uint8_t*) { ps_spu_stop(ctx, kKernelReturn, kKernelExitStop); }
void stop_exit(SPUContext* ctx, uint8_t*) { ps_spu_stop(ctx, kKernelExit, kKernelExitStop); }
void stop_job_return(SPUContext* ctx, uint8_t*) { ps_spu_stop(ctx, ls::jc_return, kJobReturnStop); }

// One 4-byte program per stop: a wider one would also hold the policy module's code (from 0xA00), and the run loop
// would not go back to the module's own program from it.
const ps_spu_entry kSelectEntry[] = {{kKernelSelect, &stop_select}};
const ps_spu_entry kReturnEntry[] = {{kKernelReturn, &stop_return}};
const ps_spu_entry kExitEntry[] = {{kKernelExit, &stop_exit}};
const ps_spu_entry kJobReturnEntry[] = {{ls::jc_return, &stop_job_return}};
std::array<ps_spu_program, 4> g_stubs{{
    {"spurs_stop_select", 0x5350555253000001ull, kKernelSelect, 4, PS_SPU_XFLOAT_APPROXIMATE, 0, 1, kSelectEntry, nullptr,
        nullptr},
    {"spurs_stop_return", 0x5350555253000002ull, kKernelReturn, 4, PS_SPU_XFLOAT_APPROXIMATE, 0, 1, kReturnEntry, nullptr,
        nullptr},
    {"spurs_stop_exit", 0x5350555253000003ull, kKernelExit, 4, PS_SPU_XFLOAT_APPROXIMATE, 0, 1, kExitEntry, nullptr,
        nullptr},
    {"spurs_stop_job_return", 0x5350555253000004ull, ls::jc_return, 4, PS_SPU_XFLOAT_APPROXIMATE, 0, 1, kJobReturnEntry,
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
    const u32 at = m.block_run[pc >> kBlockShift];
    if (!at) return nullptr;
    Run& r = m.runs[at - 1];
    if (!r.holds(pc)) return nullptr;
    const u32 size = (r.ls_end - r.ls_start) & ~3u;
    if (!r.resolved) {
        r.resolved = true;
        u8* base = ctx->membase;
        if (std::memcmp(ctx->ls + r.ls_start, base + r.ea_start, size) != 0) {
            g_modified.fetch_add(1, std::memory_order_relaxed);
            log_modified(ctx->ls + r.ls_start, base + r.ea_start, size, r.ls_start, r.ea_start);
            return nullptr;
        }
        r.hash = ps_spu_hash64(base + r.ea_start, size);
        r.program = identify_hashed(r.hash, base, {ImageKind::Module, r.ea_start, r.ls_start, size, pc});
        if (r.program) g_modules.fetch_add(1, std::memory_order_relaxed);
    }
    if (r.program) {
        *code_start = r.ls_start;
        return r.program;
    }
    if (r.hash) dump_seed(r.hash, r.ls_start, pc);
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
    u32 ls_start, ls_end, ea_start;
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
    for (const Run& r : m.runs)
        if (r.ls_end > r.ls_start && keep(r.ls_start, r.ls_end)) runs.push_back({r.ls_start, r.ls_end, r.ea_start});
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
        // Identified again when execution reaches it: its guest copy is checked against local store as after a GET.
        const u32 index = take_slot(m);
        m.runs[index] = Run{saved.ls_start, saved.ls_end, saved.ea_start};
        for (u32 b = saved.ls_start >> kBlockShift; b <= (saved.ls_end - 1) >> kBlockShift; b++)
            m.block_run[b] = static_cast<u8>(index + 1);
    }
    m.last = kRuns;
}

void forget_module_runs(SPUContext& ctx)
{
    Modules& m = modules_for(&ctx);
    m = Modules{};
    m.ctx = &ctx;
}

const ps_spu_program* identify_program(u8* base, const SpuImage& image)
{
    return identify_hashed(ps_spu_hash64(base + image.guest, image.size), base, image);
}

void attach_program(SPUContext& ctx, u8* base, const SpuImage& image)
{
    ps_spu_attach_program_at(&ctx, identify_program(base, image), image.ls_start);
}

} // namespace pspocket::hle::spurs

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
