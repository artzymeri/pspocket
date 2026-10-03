// Differential fuzz: every case of the generated table (fuzz_case.h) runs on the interpreter and as lifted code
// from the same random context and memory; the whole context and the memory window must match.
//
//   PSPOCKET_FUZZ_SEED=<n>   seed of the random contexts (default 0x5EED); case encodings are fixed at build time
//   PSPOCKET_FUZZ_ITERS=<n>  contexts per case (default 1)

#include <catch_amalgamated.hpp>

#include <cstdlib>
#include <map>
#include <vector>

#include "fuzz_case.h"
#include "ppu_test_util.hpp"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/ppu/decoder.hpp"
#include "pspocket/ppu/disasm.hpp"
#include "pspocket/ppu/interpreter.hpp"
#include "pspocket/ppu/runtime.h"

using namespace pspocket;
using namespace pspocket::ppu;

namespace {

u64 g_timebase = 0;
u64 fixed_timebase() { return g_timebase; }
u64 g_traps = 0;

// Handlers that read and write registers, so a missing spill or reload around the call shows up as a mismatch.
void fuzz_syscall(PPUContext* ctx, u8*)
{
    ctx->gpr[3] = ctx->gpr[3] * 0x9E3779B97F4A7C15ull + ctx->gpr[11] + ctx->cia;
    ctx->gpr[11] ^= ctx->lr;
    ctx->cr[2] ^= 1;
    ctx->ctr += 7;
}

void fuzz_trap(PPUContext* ctx, u8*, u32 pc)
{
    g_traps++;
    ctx->gpr[4] ^= 0xA5A5000000000000ull ^ pc ^ ctx->gpr[5];
    ctx->cr[31] ^= 1;
    ctx->ctr += 3;
    ctx->fpr[1] = static_cast<double>(pc);
}

u64 env_u64(const char* name, u64 fallback)
{
    const char* v = std::getenv(name);
    return v ? std::strtoull(v, nullptr, 0) : fallback;
}

void apply_overrides(PPUContext& ctx, u8* base, const ps_fuzz_case& c)
{
    for (u32 k = 0; k < c.override_count; k++) {
        const ps_fuzz_override& o = ps_fuzz_overrides[c.first_override + k];
        switch (o.kind) {
        case PS_FZ_GPR: ctx.gpr[o.index] = o.value; break;
        case PS_FZ_FPR: ctx.fpr[o.index] = std::bit_cast<double>(o.value); break;
        case PS_FZ_CTR: ctx.ctr = o.value; break;
        case PS_FZ_XER_BC: ctx.xer_bc = static_cast<u8>(o.value); break;
        case PS_FZ_RES_MATCH:
        case PS_FZ_RES_STALE_VALUE:
        case PS_FZ_RES_OTHER_ADDR: {
            const u32 addr = static_cast<u32>(o.value);
            ctx.reserve_valid = 1;
            ctx.reserve_size = o.index;
            ctx.reserve_addr = addr;
            ctx.reserve_value = o.index == 8 ? ps_read64(base, addr) : ps_read32(base, addr);
            if (o.kind == PS_FZ_RES_STALE_VALUE) ctx.reserve_value ^= 1;
            if (o.kind == PS_FZ_RES_OTHER_ADDR) ctx.reserve_addr ^= 0x10;
            break;
        }
        case PS_FZ_RES_NONE: ctx.reserve_valid = 0; break;
        case PS_FZ_XER_CA: ctx.xer_ca = static_cast<u8>(o.value); break;
        default: FAIL("unknown override kind");
        }
    }
}

struct HookGuard {
    HookGuard()
    {
        ps_ppu_set_syscall_handler(fuzz_syscall);
        ps_ppu_set_trap_handler(fuzz_trap);
        ps_ppu_set_timebase_source(fixed_timebase);
    }
    ~HookGuard()
    {
        ps_ppu_set_syscall_handler(nullptr);
        ps_ppu_set_trap_handler(nullptr);
        ps_ppu_set_timebase_source(nullptr);
    }
};

} // namespace

TEST_CASE("every instruction form the interpreter supports has at least 200 fuzz cases")
{
    std::map<std::pair<u32, u32>, u32> counts;
    for (u32 i = 0; i < ps_fuzz_case_count; i++) counts[{ps_fuzz_cases[i].op, ps_fuzz_cases[i].variant}]++;

    const std::vector<Form> forms = supported_forms();
    CHECK(forms.size() > 300);
    CHECK(ps_fuzz_cases_per_form >= 200);
    for (const Form& f : forms) {
        INFO("form " << form_name(f.op, f.variant));
        CHECK(counts[{static_cast<u32>(f.op), f.variant}] >= 200);
    }
    // ... and nothing else: every case belongs to a supported form.
    CHECK(counts.size() == forms.size());
}

TEST_CASE("differential fuzz: interpreter and lifted code agree on every case")
{
    GuestMemory mem;
    REQUIRE(test::reserve_for_lifted(mem));
    REQUIRE(mem.commit(PS_FUZZ_WINDOW_BASE, PS_FUZZ_WINDOW_SIZE));
    REQUIRE(mem.commit(PS_FUZZ_CODE_BASE, PS_FUZZ_CODE_SIZE));
    u8* base = mem.base();
    HookGuard hooks;

    const u64 seed = env_u64("PSPOCKET_FUZZ_SEED", 0x5EED);
    const u64 iterations = env_u64("PSPOCKET_FUZZ_ITERS", 1);
    std::vector<u8> initial(PS_FUZZ_WINDOW_SIZE), after_interp(PS_FUZZ_WINDOW_SIZE), after_lifted(PS_FUZZ_WINDOW_SIZE);

    // Outcome counts per form (first iteration only): proves the generated cases reach both sides of the
    // interesting conditions instead of passing vacuously.
    struct Outcomes {
        u32 yes = 0, no = 0;
    };
    std::map<std::pair<u32, u32>, Outcomes> outcomes;
    auto outcome_of = [](Op op, const PPUContext& before, const PPUContext& after, u32 pc, u64 traps) -> int {
        switch (op) {
        case Op::BC:
        case Op::BCLR:
        case Op::BCCTR: return after.cia != pc + 4;
        case Op::STWCX_:
        case Op::STDCX_: return after.cr[PS_CR_EQ];
        case Op::TW:
        case Op::TWI:
        case Op::TD:
        case Op::TDI: return traps != 0;
        default:
            if ((op_info(op).flags & OPF_OE) && before.xer_ov == 0) return after.xer_ov; // OV produced
            return -1;
        }
    };

    u64 failures = 0, executed = 0;
    for (u64 it = 0; it < iterations; it++) {
        for (u32 i = 0; i < ps_fuzz_case_count; i++) {
            const ps_fuzz_case& c = ps_fuzz_cases[i];
            test::Rng rng(seed * 0x2545F4914F6CDD1Dull + i + (it << 40));

            PPUContext interp;
            test::random_context(interp, rng);
            for (u32 off = 0; off < PS_FUZZ_WINDOW_SIZE; off += 8) ps_write64(base, PS_FUZZ_WINDOW_BASE + off, rng.next());
            apply_overrides(interp, base, c);
            g_timebase = rng.next();
            mem.read_bytes(PS_FUZZ_WINDOW_BASE, initial.data(), initial.size());
            PPUContext lifted = interp;

            ps_write32(base, c.pc, c.insn);
            interp.cia = c.pc;
            const PPUContext before = interp;
            g_traps = 0;
            step(interp, base);
            if (it == 0) {
                const bool oe_form = (c.variant & kVarOe) != 0;
                const int o = outcome_of(static_cast<Op>(c.op), before, interp, c.pc, g_traps);
                if (o >= 0 && (oe_form || !(op_info(static_cast<Op>(c.op)).flags & OPF_OE))) {
                    Outcomes& oc = outcomes[{c.op, c.variant}];
                    (o ? oc.yes : oc.no)++;
                }
            }
            mem.read_bytes(PS_FUZZ_WINDOW_BASE, after_interp.data(), after_interp.size());

            mem.write_bytes(PS_FUZZ_WINDOW_BASE, initial.data(), initial.size());
            lifted.cia = c.pc;
            c.fn(&lifted, base);
            mem.read_bytes(PS_FUZZ_WINDOW_BASE, after_lifted.data(), after_lifted.size());
            executed++;

            const bool relaxed = (op_info(static_cast<Op>(c.op)).flags & OPF_FPARITH) != 0;
            std::string diff = test::diff_contexts(interp, lifted, relaxed);
            for (u32 b = 0; b < PS_FUZZ_WINDOW_SIZE; b++) {
                if (after_interp[b] != after_lifted[b]) {
                    diff += std::format(" memory[{:#x}]: {:#04x} vs {:#04x};", b, after_interp[b], after_lifted[b]);
                    break;
                }
            }
            if (!diff.empty()) {
                if (++failures <= 25) {
                    UNSCOPED_INFO(std::format("case {} ({}) {:08x} '{}' at {:#x}, seed {:#x} iteration {}:{}", i,
                        form_name(static_cast<Op>(c.op), c.variant), c.insn, disassemble(c.insn, c.pc), c.pc, seed, it, diff));
                    CHECK(diff.empty());
                }
            }
        }
    }
    INFO(failures << " mismatches in " << executed << " executions");
    CHECK(failures == 0);
    CHECK(executed == u64{ps_fuzz_case_count} * iterations);

    // Every branch/stwcx./trap form goes both ways; every OE form overflows sometimes.
    CHECK(outcomes.size() > 40);
    for (const auto& [form, oc] : outcomes) {
        const Op op = static_cast<Op>(form.first);
        INFO("form " << form_name(op, form.second) << ": " << oc.yes << " yes / " << oc.no << " no");
        CHECK(oc.yes >= 10);
        if (!(op_info(op).flags & OPF_OE)) CHECK(oc.no >= 10);
    }
}
