// SPU runtime: program key hash, lifted-program registry, miss policy and the run loop that switches between
// lifted code and the interpreter.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "internal.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/spu/fp.h"
#include "pspocket/spu/interpreter.hpp"
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

constexpr u64 kP1 = 0x9E3779B185EBCA87ull;
constexpr u64 kP2 = 0xC2B2AE3D27D4EB4Full;
constexpr u64 kP3 = 0x165667B19E3779F9ull;

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
    if (g_logged_misses.insert(key).second)
        PS_LOG_WARN("spu", "SPU {}: no lifted code at 0x{:05x} in program {} ({:016x}, LS 0x{:05x}): interpreting", ctx->id,
            at, p->name, p->hash, pc);
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
    ctx->program = p;
    ctx->reloc = reloc;
    if (p) ctx->xfloat = p->xfloat;
}

bool resolve_program(SPUContext* ctx, u32 pc)
{
    if (const ps_spu_program* p = ctx->program; p && holds(p, ctx->reloc, pc)) return false; // its own code
    // Back in the code of the program the host attached, from a module's.
    if (const ps_spu_program* h = ctx->home; h && holds(h, ctx->home_reloc, pc)) {
        set_current(ctx, h, ctx->home_reloc);
        return ps_spu_lookup(ctx, pc) != nullptr;
    }
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

void ps_spu_register_program(ps_spu_program* program)
{
    PS_CHECK(program && (program->code_start & 3) == 0 && (program->code_size & 3) == 0);
    PS_CHECK(program->code_start + program->code_size <= PS_SPU_LS_SIZE);
    std::lock_guard lock(g_registry_mutex);
    if (!program->table) {
        program->table = new ps_spu_fn[program->code_size / 4]();
        for (u32 i = 0; i < program->entry_count; i++) {
            const ps_spu_entry& e = program->entries[i];
            const u32 off = e.addr - program->code_start;
            PS_CHECK(off < program->code_size && (off & 3) == 0);
            program->table[off >> 2] = e.fn;
        }
    }
    if (std::find(g_programs.begin(), g_programs.end(), program) == g_programs.end()) g_programs.push_back(program);
}

void ps_spu_unregister_program(ps_spu_program* program)
{
    std::lock_guard lock(g_registry_mutex);
    g_programs.erase(std::remove(g_programs.begin(), g_programs.end(), program), g_programs.end());
    delete[] program->table;
    program->table = nullptr;
}

const ps_spu_program* ps_spu_find_program(uint64_t hash, uint32_t code_start, uint32_t code_size)
{
    std::lock_guard lock(g_registry_mutex);
    for (const ps_spu_program* p : g_programs)
        if (p->hash == hash && p->code_size == code_size && (p->relocatable || p->code_start == code_start)) return p;
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
    const u32 reloc = detail::reloc_for(program, code_start);
    detail::set_current(ctx, program, reloc);
    ctx->home = program;
    ctx->home_reloc = reloc;
}

void ps_spu_set_miss_policy(ps_spu_miss_policy policy) { g_miss_policy.store(policy); }
void ps_spu_set_resolver(ps_spu_resolver resolver) { g_resolver.store(resolver, std::memory_order_release); }
void ps_spu_set_get_observer(ps_spu_get_observer observer) { g_get_observer.store(observer, std::memory_order_release); }
ps_spu_miss_policy ps_spu_get_miss_policy(void) { return g_miss_policy.load(); }
uint64_t ps_spu_miss_count(void) { return g_miss_count.load(); }

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
            options.stop_at_lifted = ctx->program || ctx->home || g_resolver.load(std::memory_order_relaxed);
            run(*ctx, options);
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
