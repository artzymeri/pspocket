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

#include "pspocket/core/log.hpp"
#include "pspocket/spu/runtime.h"

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

std::atomic<u64> g_attached{0}, g_unknown{0}, g_images{0}, g_modules{0};
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
constexpr u32 kRuns = 64;

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
        index = m.next;
        m.next = (m.next + 1) % kRuns;
        m.runs[index] = Run{lsa, end, eal};
    }
    m.last = index;
    for (u32 b = lsa >> kBlockShift; b <= (end - 1) >> kBlockShift; b++) m.block_run[b] = static_cast<u8>(index + 1);
}

// The resolver: the lifted program of the module holding pc, and where it sits (its lifted programs are relocatable).
const ps_spu_program* resolve(SPUContext* ctx, uint32_t pc, uint32_t* code_start)
{
    Modules& m = modules_for(ctx);
    pc &= PS_SPU_LS_MASK & ~3u;
    const u32 at = m.block_run[pc >> kBlockShift];
    if (!at) return nullptr;
    Run& r = m.runs[at - 1];
    if (!r.holds(pc)) return nullptr;
    const u32 size = (r.ls_end - r.ls_start) & ~3u;
    if (!r.resolved) {
        r.resolved = true;
        u8* base = ctx->membase;
        if (std::memcmp(ctx->ls + r.ls_start, base + r.ea_start, size) != 0) {
            PS_LOG_DEBUG(kChan, "SPU module at LS 0x{:05x}+{:#x} (guest {:#x}) differs from its guest copy: interpreted",
                r.ls_start, size, r.ea_start);
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

} // namespace

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
    ps_spu_set_get_observer(&spurs::observe_get);
    ps_spu_set_resolver(&spurs::resolve);
}

SpuProgramStats spu_program_stats()
{
    return {spurs::g_attached.load(std::memory_order_relaxed), spurs::g_unknown.load(std::memory_order_relaxed),
        spurs::g_images.load(std::memory_order_relaxed), spurs::g_modules.load(std::memory_order_relaxed)};
}

} // namespace pspocket::hle
