// PPU runtime: dispatch table, interpreter fallbacks for lifted code, system hooks, reservations, string ops.

#include "pspocket/ppu/runtime.h"

#include <algorithm>
#include <atomic>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/core/timebase.h"
#include "pspocket/ppu/disasm.hpp"
#include "pspocket/ppu/interpreter.hpp"

using namespace pspocket;

extern "C" {
ps_ppu_dispatch_table ps_ppu_dispatch = {nullptr, 0, 0};
}

namespace {

constexpr std::string_view kChan = "ppu";

// Host-side slot storage behind ps_ppu_dispatch.slots. Only the registration functions touch it.
std::vector<ps_ppu_fn>& slot_storage()
{
    static std::vector<ps_ppu_fn> slots;
    return slots;
}

usize g_registered = 0;

// Largest code span one table may cover (64M slots, 512 MB of host memory). PRX modules far from the main
// executable will need a second table when they are lifted (Phase 2+).
constexpr u64 kMaxSpan = 256ull << 20;

std::atomic<int> g_policy{
#ifdef NDEBUG
    PS_PPU_MISS_INTERPRET
#else
    PS_PPU_MISS_ABORT
#endif
};
std::atomic<u64> g_misses{0};

bool should_log_miss(u64 n) { return n <= 32 || (n % 4096) == 0; }

// Makes the table cover [lo, hi). Grows geometrically so one-by-one registration stays linear.
void ensure_range(u32 lo, u64 hi)
{
    ps_ppu_dispatch_table& d = ps_ppu_dispatch;
    auto& storage = slot_storage();
    const u64 cur_lo = d.base;
    const u64 cur_hi = u64{d.base} + 4ull * d.count;
    if (d.count != 0 && lo >= cur_lo && hi <= cur_hi) return;

    u64 new_lo = d.count ? std::min<u64>(lo, cur_lo) : lo;
    u64 new_hi = d.count ? std::max<u64>(hi, cur_hi) : hi;
    if (d.count) {
        const u64 span = cur_hi - cur_lo;
        if (new_lo < cur_lo) new_lo = std::min<u64>(new_lo, cur_lo > span ? cur_lo - span : 0);
        if (new_hi > cur_hi) new_hi = std::max<u64>(new_hi, std::min<u64>(cur_hi + span, 0x100000000ull));
    }
    new_lo &= ~0xFFFFull; // 64 KB granules
    new_hi = std::min<u64>((new_hi + 0xFFFF) & ~0xFFFFull, 0x100000000ull);
    if (new_hi - new_lo > kMaxSpan) {
        PS_FATAL(kChan, "dispatch table would span {:#x}..{:#x} ({} MB of guest code); more than one code range is "
                        "not supported yet", new_lo, new_hi, (new_hi - new_lo) >> 20);
    }

    std::vector<ps_ppu_fn> grown(static_cast<usize>((new_hi - new_lo) / 4), ps_ppu_dispatch_miss_stub);
    if (d.count) std::copy(storage.begin(), storage.end(), grown.begin() + static_cast<std::ptrdiff_t>((cur_lo - new_lo) / 4));
    storage.swap(grown);
    d.slots = storage.data();
    d.base = static_cast<u32>(new_lo);
    d.count = static_cast<u32>(storage.size());
}

void default_syscall(PPUContext* ctx, u8*)
{
    PS_FATAL(kChan, "syscall {} at {:#010x}: no syscall handler installed (lv2 arrives in Phase 2)", ctx->gpr[11],
        ctx->cia);
}

void default_trap(PPUContext* ctx, u8* base, u32 pc)
{
    const u32 word = ps_read32(base, pc);
    PS_FATAL(kChan, "trap at {:#010x}: {:08x} '{}' (LR {:#x})", pc, word, ppu::disassemble(word, pc), ctx->lr);
}

std::atomic<ps_ppu_syscall_handler> g_syscall{default_syscall};
std::atomic<ps_ppu_trap_handler> g_trap{default_trap};
std::atomic<ps_ppu_timebase_source> g_timebase{nullptr};

} // namespace

// ---------------------------------------------------------------------------------------------- dispatch table

extern "C" ps_ppu_fn ps_ppu_dispatch_find(uint32_t addr)
{
    if (addr & 3) return nullptr;
    const ps_ppu_fn fn = ps_ppu_dispatch_slot(addr);
    return fn == ps_ppu_dispatch_miss_stub ? nullptr : fn;
}

extern "C" void ps_ppu_dispatch_register(uint32_t addr, ps_ppu_fn fn)
{
    PS_CHECK((addr & 3) == 0);
    PS_CHECK(fn != nullptr && fn != ps_ppu_dispatch_miss_stub);
    ensure_range(addr, u64{addr} + 4);
    ps_ppu_fn& slot = ps_ppu_dispatch.slots[(addr - ps_ppu_dispatch.base) >> 2];
    if (slot == ps_ppu_dispatch_miss_stub) g_registered++;
    slot = fn;
}

extern "C" void ps_ppu_dispatch_register_table(const ps_ppu_dispatch_entry* entries, size_t count)
{
    if (count == 0) return;
    u32 lo = ~0u;
    u64 hi = 0;
    for (size_t i = 0; i < count; i++) {
        lo = std::min(lo, entries[i].addr);
        hi = std::max<u64>(hi, u64{entries[i].addr} + 4);
    }
    ensure_range(lo, hi);
    for (size_t i = 0; i < count; i++) ps_ppu_dispatch_register(entries[i].addr, entries[i].fn);
}

extern "C" void ps_ppu_dispatch_unregister(uint32_t addr)
{
    if (ps_ppu_dispatch_find(addr) == nullptr) return;
    ps_ppu_dispatch.slots[(addr - ps_ppu_dispatch.base) >> 2] = ps_ppu_dispatch_miss_stub;
    g_registered--;
}

extern "C" void ps_ppu_dispatch_clear(void)
{
    std::vector<ps_ppu_fn>().swap(slot_storage());
    ps_ppu_dispatch = {nullptr, 0, 0};
    g_registered = 0;
}

extern "C" size_t ps_ppu_dispatch_count(void) { return g_registered; }

extern "C" void ps_ppu_set_miss_policy(ps_ppu_miss_policy policy) { g_policy = policy; }
extern "C" ps_ppu_miss_policy ps_ppu_get_miss_policy(void) { return static_cast<ps_ppu_miss_policy>(g_policy.load()); }
extern "C" uint64_t ps_ppu_dispatch_miss_count(void) { return g_misses.load(); }

extern "C" void ps_ppu_dispatch_miss_stub(PPUContext* ctx, uint8_t* base)
{
    const u32 target = ctx->cia;
    const u64 n = ++g_misses;
    if (ps_ppu_get_miss_policy() == PS_PPU_MISS_ABORT)
        PS_FATAL(kChan, "indirect call to {:#010x} has no lifted function (LR {:#x}); add it as a seed", target, ctx->lr);
    if (should_log_miss(n))
        PS_LOG_WARN(kChan, "indirect call to {:#010x} has no lifted function (LR {:#x}): interpreting", target, ctx->lr);
    ps_ppu_interp_until(ctx, base, target, ctx->lr, ctx->gpr[1]);
}

extern "C" void ps_ppu_jump_miss(PPUContext* ctx, uint8_t* base, uint32_t target, uint64_t stop_lr, uint64_t stop_sp)
{
    const u64 n = ++g_misses;
    if (ps_ppu_get_miss_policy() == PS_PPU_MISS_ABORT)
        PS_FATAL(kChan, "indirect jump to {:#010x} has no lifted function (function LR {:#x}); add it as a seed or "
                        "teach the analyzer its jump table", target, stop_lr);
    if (should_log_miss(n))
        PS_LOG_WARN(kChan, "indirect jump to {:#010x} has no lifted function: interpreting until return to {:#x}",
            target, stop_lr);
    ps_ppu_interp_until(ctx, base, target, stop_lr, stop_sp);
}

extern "C" void ps_ppu_jump_table_miss(PPUContext* ctx, uint8_t* base, uint32_t pc, uint32_t target, uint64_t stop_lr,
    uint64_t stop_sp)
{
    const u64 n = ++g_misses;
    if (ps_ppu_get_miss_policy() == PS_PPU_MISS_ABORT)
        PS_FATAL(kChan, "jump table at {:#010x}: target {:#010x} is not in the recognised table", pc, target);
    if (should_log_miss(n))
        PS_LOG_WARN(kChan, "jump table at {:#010x}: target {:#010x} is not in the recognised table", pc, target);
    ps_ppu_jump_indirect(ctx, base, target, stop_lr, stop_sp);
}

// ---------------------------------------------------------------------------------------------- fallbacks

extern "C" void ps_ppu_interp_one(PPUContext* ctx, uint8_t* base, uint32_t pc)
{
    ctx->cia = pc;
    ppu::step(*ctx, base);
}

extern "C" void ps_ppu_interp_until(PPUContext* ctx, uint8_t* base, uint32_t pc, uint64_t stop_lr, uint64_t stop_sp)
{
    ctx->cia = pc;
    ppu::RunOptions options;
    options.stop_at_return = true;
    options.stop_lr = stop_lr;
    options.stop_sp = stop_sp;
    options.call_lifted = true;
    ppu::run(*ctx, base, options);
}

extern "C" void ps_ppu_illegal(PPUContext* ctx, uint8_t* base, uint32_t pc)
{
    const u32 word = ps_read32(base, pc);
    PS_FATAL(kChan, "illegal instruction at {:#010x}: {:08x} '{}' (LR {:#x})", pc, word, ppu::disassemble(word, pc),
        ctx->lr);
}

// ---------------------------------------------------------------------------------------------- hooks

extern "C" void ps_ppu_set_syscall_handler(ps_ppu_syscall_handler handler)
{
    g_syscall = handler ? handler : default_syscall;
}

extern "C" void ps_ppu_set_trap_handler(ps_ppu_trap_handler handler) { g_trap = handler ? handler : default_trap; }

extern "C" void ps_ppu_syscall(PPUContext* ctx, uint8_t* base) { g_syscall.load(std::memory_order_relaxed)(ctx, base); }

extern "C" void ps_ppu_trap(PPUContext* ctx, uint8_t* base, uint32_t pc)
{
    g_trap.load(std::memory_order_relaxed)(ctx, base, pc);
}

extern "C" void ps_ppu_set_timebase_source(ps_ppu_timebase_source source) { g_timebase = source; }

extern "C" uint64_t ps_ppu_timebase(void)
{
    const ps_ppu_timebase_source source = g_timebase.load(std::memory_order_relaxed);
    return source ? source() : ps_timebase_now();
}

// ---------------------------------------------------------------------------------------------- reservations
// PHASE 3 HOOK: single-thread model, see runtime.h.

extern "C" uint32_t ps_ppu_lwarx(PPUContext* ctx, uint8_t* base, uint32_t ea)
{
    const u32 value = ps_read32(base, ea);
    ctx->reserve_addr = ea;
    ctx->reserve_valid = 1;
    ctx->reserve_size = 4;
    ctx->reserve_value = value;
    return value;
}

extern "C" uint64_t ps_ppu_ldarx(PPUContext* ctx, uint8_t* base, uint32_t ea)
{
    const u64 value = ps_read64(base, ea);
    ctx->reserve_addr = ea;
    ctx->reserve_valid = 1;
    ctx->reserve_size = 8;
    ctx->reserve_value = value;
    return value;
}

extern "C" int ps_ppu_stwcx(PPUContext* ctx, uint8_t* base, uint32_t ea, uint32_t value)
{
    const bool ok = ctx->reserve_valid && ctx->reserve_size == 4 && ctx->reserve_addr == ea && (ea & 3) == 0 &&
        ps_read32(base, ea) == static_cast<u32>(ctx->reserve_value);
    ctx->reserve_valid = 0;
    if (ok) ps_write32(base, ea, value);
    return ok ? 1 : 0;
}

extern "C" int ps_ppu_stdcx(PPUContext* ctx, uint8_t* base, uint32_t ea, uint64_t value)
{
    const bool ok = ctx->reserve_valid && ctx->reserve_size == 8 && ctx->reserve_addr == ea && (ea & 7) == 0 &&
        ps_read64(base, ea) == ctx->reserve_value;
    ctx->reserve_valid = 0;
    if (ok) ps_write64(base, ea, value);
    return ok ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------- string ops
// rpcs3/Emu/Cell/PPUInterpreter.cpp LSWX / STSWX.

extern "C" void ps_ppu_lswx(PPUContext* ctx, uint8_t* base, uint32_t ea, uint32_t rt)
{
    u32 count = ctx->xer_bc & 0x7f;
    u32 reg = rt & 31;
    u32 addr = ea;
    for (; count >= 4; count -= 4, addr += 4, reg = (reg + 1) & 31) ctx->gpr[reg] = ps_read32(base, addr);
    if (count) {
        u32 value = 0;
        for (u32 b = 0; b < count; b++) value |= u32{ps_read8(base, addr + b)} << ((3 ^ b) * 8);
        ctx->gpr[reg] = value;
    }
}

extern "C" void ps_ppu_stswx(PPUContext* ctx, uint8_t* base, uint32_t ea, uint32_t rs)
{
    u32 count = ctx->xer_bc & 0x7f;
    u32 reg = rs & 31;
    u32 addr = ea;
    for (; count >= 4; count -= 4, addr += 4, reg = (reg + 1) & 31) ps_write32(base, addr, static_cast<u32>(ctx->gpr[reg]));
    if (count) {
        const u32 value = static_cast<u32>(ctx->gpr[reg]);
        for (u32 b = 0; b < count; b++) ps_write8(base, addr + b, static_cast<u8>(value >> ((3 ^ b) * 8)));
    }
}
