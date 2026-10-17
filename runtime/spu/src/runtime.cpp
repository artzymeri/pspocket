// SPU runtime: program key hash, lifted-program registry, miss policy and the run loop that switches between
// lifted code and the interpreter.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "internal.hpp"
#include "pspocket/spu/diag.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/spu/fp.h"
#include "pspocket/spu/interpreter.hpp"
#include "pspocket/spu/registry.hpp"
#include "pspocket/spu/replace.hpp"
#include "pspocket/spu/runtime.h"

using namespace pspocket;
using namespace pspocket::spu;

namespace {

// The context ps_spu_run / ps_spu_interpret is running on this thread (ps_spu_current); nested runs restore the outer.
thread_local SPUContext* t_current = nullptr;
struct CurrentContext {
    explicit CurrentContext(SPUContext* ctx) : previous(t_current) { t_current = ctx; }
    ~CurrentContext() { t_current = previous; }
    CurrentContext(const CurrentContext&) = delete;
    CurrentContext& operator=(const CurrentContext&) = delete;
    SPUContext* previous;
};

static_assert(std::endian::native == std::endian::little, "ps_spu_hash64 reads words in little-endian order");

std::mutex g_registry_mutex;
std::vector<ps_spu_program*> g_programs;
// The tables of programs unregistered: a context may still run one, so they are never freed (D-231).
std::vector<ps_spu_fn*> g_retired_tables;
std::atomic<u64> g_registry_generation{0};
std::atomic<u64> g_registry_additions{0}, g_registry_removals{0};
// RegistryBatch: how many live on this thread, and whether a registration was made under them.
thread_local u32 t_batch_depth = 0;
thread_local bool t_batch_added = false;

#ifdef NDEBUG
std::atomic<ps_spu_miss_policy> g_miss_policy{PS_SPU_MISS_INTERPRET};
#else
std::atomic<ps_spu_miss_policy> g_miss_policy{PS_SPU_MISS_ABORT};
#endif
std::atomic<u64> g_miss_count{0};
std::atomic<ps_spu_resolver> g_resolver{nullptr};
std::atomic<ps_spu_get_observer> g_get_observer{nullptr};
std::mutex g_miss_mutex;
std::unordered_set<u64> g_logged_misses;
std::FILE* g_miss_log = nullptr; // ps_spu_set_miss_log (under g_miss_mutex)

constexpr u64 kP1 = 0x9E3779B185EBCA87ull;
constexpr u64 kP2 = 0xC2B2AE3D27D4EB4Full;
constexpr u64 kP3 = 0x165667B19E3779F9ull;

// ps_spu_interp_sites: an open-addressed table of (program, pc) keys, filled without locks.
constexpr u32 kSites = 4096;
struct Site {
    std::atomic<const ps_spu_program*> program{nullptr};
    std::atomic<u32> pc{~0u};
    std::atomic<u64> steps{0}, entries{0};
};
Site g_sites[kSites];

// The interpreted steps of contexts running an image with no lifted program, by image (D-233): (hash, LS start) keys,
// filled without locks as g_sites.
struct ImageSite {
    std::atomic<u64> hash{0};
    std::atomic<u32> ls_start{~0u};
    std::atomic<u64> steps{0};
};
constexpr u32 kImageSites = 1024;
ImageSite g_image_sites[kImageSites];
std::atomic<u64> g_pending_adopted{0};
// The B translation (D-962): each default program's twin, set at startup before SPU threads run, read without a lock.
std::unordered_map<const ps_spu_program*, const ps_spu_program*> g_twin_b;
std::atomic<bool> g_have_b{false};
thread_local bool t_translation_b = false;
std::mutex g_pending_log_mutex;
std::unordered_set<u64> g_pending_logged;

void count_image(u64 hash, u32 ls_start, u64 steps)
{
    u32 h = static_cast<u32>((hash ^ (hash >> 29) ^ ls_start * 0x85EBCA6Bu) % kImageSites);
    for (u32 probe = 0; probe < 32; probe++, h = (h + 1) % kImageSites) {
        ImageSite& s = g_image_sites[h];
        u32 expected = ~0u;
        if (s.ls_start.load(std::memory_order_relaxed) == ~0u && s.ls_start.compare_exchange_strong(expected, ls_start)) {
            s.hash.store(hash, std::memory_order_relaxed);
        } else if (s.ls_start.load(std::memory_order_relaxed) != ls_start || s.hash.load(std::memory_order_relaxed) != hash) {
            continue;
        }
        s.steps.fetch_add(steps, std::memory_order_relaxed);
        return;
    }
}

void count_interpreted(const ps_spu_program* program, u32 pc, u64 steps)
{
    u32 h = static_cast<u32>((reinterpret_cast<uintptr_t>(program) >> 4) * 0x9E3779B1u ^ pc * 0x85EBCA6Bu) % kSites;
    for (u32 probe = 0; probe < 32; probe++, h = (h + 1) % kSites) {
        Site& s = g_sites[h];
        u32 expected = ~0u;
        if (s.pc.load(std::memory_order_relaxed) == ~0u && s.pc.compare_exchange_strong(expected, pc)) {
            s.program.store(program, std::memory_order_relaxed);
        } else if (s.pc.load(std::memory_order_relaxed) != pc || s.program.load(std::memory_order_relaxed) != program) {
            continue;
        }
        s.steps.fetch_add(steps, std::memory_order_relaxed);
        s.entries.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

void report_miss(const SPUContext* ctx, u32 pc)
{
    g_miss_count.fetch_add(1, std::memory_order_relaxed);
    const ps_spu_program* p = ctx->program;
    // The address in the program's own terms (where it was lifted): scripts/lift-spu.sh takes it as a seed.
    const u32 at = (pc - ctx->reloc) & 0x3FFFFu;
    if (g_miss_policy.load(std::memory_order_relaxed) == PS_SPU_MISS_ABORT)
        PS_FATAL("spu", "SPU {}: no lifted code at 0x{:05x} in program {} ({:016x}, LS 0x{:05x})", ctx->id, at, p->name,
            p->hash, pc);
    const u64 key = (p->hash << 18) ^ at;
    std::lock_guard lock(g_miss_mutex);
    if (g_logged_misses.insert(key).second) {
        PS_LOG_WARN("spu", "SPU {}: no lifted code at 0x{:05x} in program {} ({:016x}, LS 0x{:05x}): interpreting", ctx->id,
            at, p->name, p->hash, pc);
        if (g_miss_log) {
            std::fprintf(g_miss_log, "SPU %u: no lifted code at 0x%05x in program %s (%016llx, LS 0x%05x): interpreting\n",
                static_cast<unsigned>(ctx->id), at, p->name, static_cast<unsigned long long>(p->hash), pc);
            std::fflush(g_miss_log);
        }
    }
}

} // namespace

namespace pspocket::spu::detail {

bool holds(const ps_spu_program* p, u32 reloc, u32 pc) { return ((pc - reloc - p->code_start) & 0x3FFFFu) < p->code_size; }

u32 reloc_for(const ps_spu_program* p, u32 code_start)
{
    return p && p->relocatable ? (code_start - p->code_start) & 0x3FFFFu : 0;
}

void set_current(SPUContext* ctx, const ps_spu_program* p, u32 reloc)
{
    if (t_translation_b && p)
        if (const auto it = g_twin_b.find(p); it != g_twin_b.end()) p = it->second;
    ctx->program = p;
    ctx->reloc = reloc;
    if (p) ctx->xfloat = p->xfloat;
}

// A context running an image with no lifted program takes the program registered for it since (D-233): looked up
// once per batch of registrations, attached if its code words in local store are still the program's code and it was
// lifted for the floating-point model the context runs with (switching models mid-run would change results).
bool adopt_pending(SPUContext* ctx)
{
    ps_spu_pending_home& pend = ctx->host->pending;
    const u64 additions = pspocket::spu::registry_additions();
    if (additions == pend.checked) return false;
    pend.checked = additions;
    const ps_spu_program* p =
        pspocket::spu::find_program_checked(pend.hash, pend.ls_start, pend.size, pend.relocatable, ctx->ls);
    if (!p) return false;
    if (p->xfloat != ctx->xfloat) {
        pend.armed = false; // its next load attaches it
        std::lock_guard lock(g_pending_log_mutex);
        if (g_pending_logged.insert(pend.hash).second)
            PS_LOG_WARN("spu", "SPU {}: program {} registered for the image it runs, lifted for another floating-point "
                               "model: attached at its next load", ctx->id, p->name);
        return false;
    }
    ps_spu_attach_program_at(ctx, p, pend.ls_start);
    g_pending_adopted.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool resolve_program(SPUContext* ctx, u32 pc)
{
    if (const ps_spu_program* p = ctx->program; p && holds(p, ctx->reloc, pc)) return false; // its own code
    // Back in the code of the program the host attached, from a module's.
    if (const ps_spu_program* h = ctx->home; h && holds(h, ctx->home_reloc, pc)) {
        set_current(ctx, h, ctx->home_reloc);
        return ps_spu_lookup(ctx, pc) != nullptr;
    }
    if (!ctx->home && pending_armed(ctx) && adopt_pending(ctx) && holds(ctx->home, ctx->home_reloc, pc))
        return ps_spu_lookup(ctx, pc) != nullptr;
    const ps_spu_resolver resolver = g_resolver.load(std::memory_order_relaxed);
    if (!resolver) return false;
    u32 code_start = 0;
    const ps_spu_program* other = resolver(ctx, pc, &code_start);
    if (!other) return false;
    set_current(ctx, other, reloc_for(other, code_start));
    return ps_spu_lookup(ctx, pc) != nullptr;
}

void observe_get(SPUContext* ctx, u32 lsa, u32 eal, u32 size)
{
    if (const ps_spu_get_observer observer = g_get_observer.load(std::memory_order_relaxed)) observer(ctx, lsa, eal, size);
}

} // namespace pspocket::spu::detail

namespace {

// Returns true when the run loop must stop.
bool handle_attention(SPUContext* ctx)
{
    const u32 attention = __atomic_load_n(&ctx->attention, __ATOMIC_ACQUIRE);
    if (attention & PS_SPU_ATTN_STOP) {
        ctx->stop_reason = PS_SPU_STOP_REQUESTED;
        return true;
    }
    if (attention & PS_SPU_ATTN_INTERRUPT) {
        __atomic_fetch_and(&ctx->attention, ~PS_SPU_ATTN_INTERRUPT, __ATOMIC_ACQ_REL);
        detail::check_interrupt(ctx);
    }
    return false;
}

u32 status_for(const SPUContext* ctx)
{
    switch (ctx->stop_reason) {
    case PS_SPU_STOP_SIGNAL: return PS_SPU_STATUS_STOPPED_BY_STOP | (ctx->stop_code << 16);
    case PS_SPU_STOP_HALT: return PS_SPU_STATUS_STOPPED_BY_HALT;
    default: return PS_SPU_STATUS_STOPPED;
    }
}

} // namespace

extern "C" {

uint64_t ps_spu_hash64(const uint8_t* data, size_t size)
{
    u64 h = kP3 ^ (static_cast<u64>(size) * kP1);
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        u64 w;
        std::memcpy(&w, data + i, 8);
        h = std::rotl(h ^ (w * kP2), 31) * kP1;
    }
    if (i < size) {
        u64 w = 0;
        std::memcpy(&w, data + i, size - i);
        h = std::rotl(h ^ (w * kP2) ^ (static_cast<u64>(size - i) << 56), 31) * kP1;
    }
    h ^= h >> 33;
    h *= kP2;
    h ^= h >> 29;
    h *= kP3;
    h ^= h >> 32;
    return h;
}

namespace {

ps_spu_fn* build_table(const ps_spu_program* program)
{
    ps_spu_fn* table = new ps_spu_fn[program->code_size / 4]();
    for (u32 i = 0; i < program->entry_count; i++) {
        const ps_spu_entry& e = program->entries[i];
        const u32 off = e.addr - program->code_start;
        PS_CHECK(off < program->code_size && (off & 3) == 0);
        table[off >> 2] = e.fn;
    }
    return table;
}

} // namespace

uint64_t ps_spu_last_dma(SPUContext* ctx, uint32_t* cmd, uint32_t* ea, uint32_t* size, uint32_t* pc)
{
    ps_spu_host* h = ctx ? ctx->host : nullptr;
    if (!h) return 0;
    *cmd = h->last_cmd.load(std::memory_order_relaxed);
    *ea = h->last_ea.load(std::memory_order_relaxed);
    *size = h->last_size.load(std::memory_order_relaxed);
    *pc = h->last_pc.load(std::memory_order_relaxed);
    return h->dmas.load(std::memory_order_relaxed);
}

void ps_spu_register_program(ps_spu_program* program)
{
    PS_CHECK(program && (program->code_start & 3) == 0 && (program->code_size & 3) == 0);
    PS_CHECK(program->code_start + program->code_size <= PS_SPU_LS_SIZE);
    // A known build of middleware (D-965): the program derived from it, its listed functions native, goes first, so
    // lookups by key find it.
    if (ps_spu_program* derived = pspocket::spu::replace::derive(program)) ps_spu_register_program(derived);
    // The table is built outside the lock (SPU threads look programs up meanwhile, D-231) and published under it,
    // before the program can be found: whoever finds it through the lock sees it whole.
    bool built_already;
    {
        std::lock_guard lock(g_registry_mutex);
        built_already = program->table != nullptr;
    }
    ps_spu_fn* table = built_already ? nullptr : build_table(program);
    {
        std::lock_guard lock(g_registry_mutex);
        if (!program->table) std::swap(program->table, table); // another thread's registration may have won
        if (std::find(g_programs.begin(), g_programs.end(), program) == g_programs.end()) {
            g_programs.push_back(program);
            if (t_batch_depth) t_batch_added = true;
            else g_registry_additions.fetch_add(1, std::memory_order_release);
        }
        g_registry_generation.fetch_add(1, std::memory_order_release);
    }
    delete[] table;
}

void ps_spu_unregister_program(ps_spu_program* program)
{
    if (ps_spu_program* derived = pspocket::spu::replace::derived_of(program)) ps_spu_unregister_program(derived);
    std::lock_guard lock(g_registry_mutex);
    const auto it = std::remove(g_programs.begin(), g_programs.end(), program);
    if (it != g_programs.end()) {
        g_programs.erase(it, g_programs.end());
        // Its table stays: a context attached to it may be running it right now (D-231). Registering it again finds
        // the table built.
        if (std::find(g_retired_tables.begin(), g_retired_tables.end(), program->table) == g_retired_tables.end())
            g_retired_tables.push_back(program->table);
        g_registry_removals.fetch_add(1, std::memory_order_release);
    }
    g_registry_generation.fetch_add(1, std::memory_order_release);
}

uint64_t ps_spu_registry_generation(void) { return g_registry_generation.load(std::memory_order_acquire); }

void ps_spu_register_program_b(ps_spu_program* program)
{
    PS_CHECK(program && (program->code_start & 3) == 0 && program->code_start + program->code_size <= PS_SPU_LS_SIZE);
    if (!program->table) program->table = build_table(program);
    std::lock_guard lock(g_registry_mutex);
    for (const ps_spu_program* p : g_programs)
        if (p->hash == program->hash && p->code_start == program->code_start && p->code_size == program->code_size &&
            p->relocatable == program->relocatable && p->xfloat == program->xfloat) {
            g_twin_b[p] = program;
            g_have_b.store(true, std::memory_order_release);
            return;
        }
    PS_LOG_WARN("spu", "B translation of {}: no program of its key registered", program->name);
}

const ps_spu_program* ps_spu_find_program(uint64_t hash, uint32_t code_start, uint32_t code_size)
{
    std::lock_guard lock(g_registry_mutex);
    for (const ps_spu_program* p : g_programs)
        if (p->hash == hash && p->code_size == code_size && (p->relocatable || p->code_start == code_start)) return p;
    return nullptr;
}

uint64_t ps_spu_code_hash(const uint8_t* image, uint32_t code_size, const uint32_t* code_words)
{
    // The code words gathered in a buffer kept per thread: SPU threads hash images as they load them (D-231).
    thread_local std::vector<u8> code;
    code.clear();
    code.reserve(code_size);
    for (u32 w = 0; w < code_size / 4; w++)
        if (!code_words || ((code_words[w >> 5] >> (w & 31)) & 1)) code.insert(code.end(), image + 4 * w, image + 4 * w + 4);
    return ps_spu_hash64(code.data(), code.size());
}

const ps_spu_program* ps_spu_find_program_by_code(const uint8_t* image, uint32_t code_size)
{
    std::vector<const ps_spu_program*> candidates;
    {
        std::lock_guard lock(g_registry_mutex);
        for (const ps_spu_program* p : g_programs)
            if (p->relocatable && p->code_hash && p->code_words && p->code_size == code_size) candidates.push_back(p);
    }
    for (const ps_spu_program* p : candidates)
        if (ps_spu_code_hash(image, code_size, p->code_words) == p->code_hash) return p;
    return nullptr;
}

const ps_spu_program* ps_spu_identify(SPUContext* ctx, uint32_t code_start, uint32_t code_size)
{
    PS_CHECK(code_start < PS_SPU_LS_SIZE && code_size <= PS_SPU_LS_SIZE - code_start);
    const u64 hash = ps_spu_hash64(ctx->ls + code_start, code_size);
    const ps_spu_program* p = ps_spu_find_program(hash, code_start, code_size);
    if (!p) {
        // CLAUDE.md rule 5: unknown SPU hash.
        if (g_miss_policy.load(std::memory_order_relaxed) == PS_SPU_MISS_ABORT)
            PS_FATAL("spu", "unknown SPU program: hash {:016x}, LS 0x{:05x}+0x{:x}", hash, code_start, code_size);
        PS_LOG_WARN("spu", "unknown SPU program: hash {:016x}, LS 0x{:05x}+0x{:x}: interpreting", hash, code_start,
            code_size);
    }
    ps_spu_attach_program(ctx, p);
    return p;
}

void ps_spu_attach_program(SPUContext* ctx, const ps_spu_program* program)
{
    ps_spu_attach_program_at(ctx, program, program ? program->code_start : 0);
}

void ps_spu_attach_program_at(SPUContext* ctx, const ps_spu_program* program, uint32_t code_start)
{
    detail::disarm_pending(ctx); // a new image (or none): a key set before is another's (D-233)
    const u32 reloc = detail::reloc_for(program, code_start);
    detail::set_current(ctx, program, reloc);
    ctx->home = program;
    ctx->home_reloc = reloc;
}

void ps_spu_set_miss_policy(ps_spu_miss_policy policy) { g_miss_policy.store(policy); }

void ps_spu_set_miss_log(const char* path)
{
    std::lock_guard lock(g_miss_mutex);
    if (g_miss_log) std::fclose(g_miss_log);
    g_miss_log = path ? std::fopen(path, "a") : nullptr;
}
void ps_spu_set_resolver(ps_spu_resolver resolver) { g_resolver.store(resolver, std::memory_order_release); }
void ps_spu_set_get_observer(ps_spu_get_observer observer) { g_get_observer.store(observer, std::memory_order_release); }
ps_spu_miss_policy ps_spu_get_miss_policy(void) { return g_miss_policy.load(); }
uint64_t ps_spu_miss_count(void) { return g_miss_count.load(); }

uint32_t ps_spu_interp_sites(ps_spu_interp_site* out, uint32_t n)
{
    std::vector<ps_spu_interp_site> all;
    for (const Site& s : g_sites) {
        if (s.pc.load(std::memory_order_relaxed) == ~0u) continue;
        all.push_back({s.program.load(std::memory_order_relaxed), s.pc.load(std::memory_order_relaxed),
            s.steps.load(std::memory_order_relaxed), s.entries.load(std::memory_order_relaxed)});
    }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.steps > b.steps; });
    const u32 count = std::min<u32>(n, static_cast<u32>(all.size()));
    std::copy_n(all.begin(), count, out);
    return count;
}

SPUContext* ps_spu_current(void) { return t_current; }

uint32_t ps_spu_run(SPUContext* ctx)
{
    const CurrentContext current(ctx);
    const u64 fp = ps_spu_fpenv_enter();
    ctx->stop_reason = PS_SPU_STOP_NONE;
    ctx->exception = PS_SPU_EXC_NONE;
    detail::set_status(ctx, PS_SPU_STATUS_RUNNING);
    for (;;) {
        if (__atomic_load_n(&ctx->attention, __ATOMIC_RELAXED) && handle_attention(ctx)) break;
        if (const ps_spu_fn fn = ps_spu_lookup(ctx, ctx->pc)) {
            fn(ctx, ctx->ls);
        } else {
            if (detail::resolve_program(ctx, ctx->pc)) continue; // another program has lifted code here (D-079)
            if (ctx->program) report_miss(ctx, ctx->pc);
            RunOptions options;
            const bool pending = detail::pending_armed(ctx);
            options.stop_at_lifted =
                ctx->program || ctx->home || pending || g_resolver.load(std::memory_order_relaxed);
            const ps_spu_program* program = ctx->program;
            const u32 pc = ctx->pc;
            const u64 steps = run(*ctx, options).steps;
            count_interpreted(program, pc, steps);
            if (pending && !program) count_image(ctx->host->pending.hash, ctx->host->pending.ls_start, steps);
        }
        if (ctx->stop_reason != PS_SPU_STOP_NONE) break;
    }
    detail::set_status(ctx, status_for(ctx));
    ps_spu_fpenv_leave(fp);
    return ctx->stop_reason;
}

uint32_t ps_spu_interpret(SPUContext* ctx, uint64_t max_steps)
{
    const CurrentContext current(ctx);
    const u64 fp = ps_spu_fpenv_enter();
    ctx->stop_reason = PS_SPU_STOP_NONE;
    ctx->exception = PS_SPU_EXC_NONE;
    detail::set_status(ctx, PS_SPU_STATUS_RUNNING);
    RunOptions options;
    options.max_steps = max_steps;
    u64 steps = 0;
    for (;;) {
        if (__atomic_load_n(&ctx->attention, __ATOMIC_RELAXED) && handle_attention(ctx)) break;
        if (max_steps) options.max_steps = max_steps - steps;
        steps += run(*ctx, options).steps;
        if (ctx->stop_reason != PS_SPU_STOP_NONE) break;
    }
    detail::set_status(ctx, status_for(ctx));
    ps_spu_fpenv_leave(fp);
    return ctx->stop_reason;
}

} // extern "C"

namespace pspocket::spu {

uint64_t registry_additions() { return g_registry_additions.load(std::memory_order_acquire); }
bool has_translation_b() { return g_have_b.load(std::memory_order_acquire); }
void set_translation_b(bool on) { t_translation_b = on; }
uint64_t registry_removals() { return g_registry_removals.load(std::memory_order_acquire); }

void set_pending_home(SPUContext* ctx, uint64_t hash, uint32_t ls_start, uint32_t size, bool relocatable,
    uint64_t additions)
{
    if (!ctx->host || ctx->home) return;
    PS_CHECK(ls_start < PS_SPU_LS_SIZE && size <= PS_SPU_LS_SIZE - ls_start);
    ctx->host->pending = {hash, ls_start, size, relocatable, true, additions};
}

uint64_t pending_homes_adopted() { return g_pending_adopted.load(std::memory_order_relaxed); }

std::vector<InterpretedImage> interpreted_images(uint32_t n)
{
    std::vector<InterpretedImage> all;
    for (const ImageSite& s : g_image_sites) {
        if (s.ls_start.load(std::memory_order_relaxed) == ~0u) continue;
        all.push_back({s.hash.load(std::memory_order_relaxed), s.ls_start.load(std::memory_order_relaxed),
            s.steps.load(std::memory_order_relaxed)});
    }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.steps > b.steps; });
    if (all.size() > n) all.resize(n);
    return all;
}

RegistryBatch::RegistryBatch() { t_batch_depth++; }

RegistryBatch::~RegistryBatch()
{
    if (--t_batch_depth == 0 && t_batch_added) {
        t_batch_added = false;
        g_registry_additions.fetch_add(1, std::memory_order_release);
    }
}

const ps_spu_program* find_program_checked(uint64_t hash, uint32_t ls_start, uint32_t size, bool relocatable,
    const uint8_t* ls)
{
    PS_CHECK(ls_start < PS_SPU_LS_SIZE && size <= PS_SPU_LS_SIZE - ls_start);
    const ps_spu_program* p = ps_spu_find_program(hash, ls_start, size);
    if (!p && relocatable) p = ps_spu_find_program_by_code(ls + ls_start, size);
    if (!p || !p->code_hash) return nullptr;
    // Its data may have changed since `hash` was taken; its code must not have.
    return ps_spu_code_hash(ls + ls_start, size, p->code_words) == p->code_hash ? p : nullptr;
}

} // namespace pspocket::spu
