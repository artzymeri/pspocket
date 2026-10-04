// SPU differential fuzz: every case of the generated table (fuzz_case.h) runs on the interpreter and as lifted code
// from the same random context, local store and guest memory; all 128 registers (bitwise), FPSCR, pc, SRR0, the
// interrupt state, the stop/exception state and every channel/MFC/event field must match, and so must the local
// store and the guest window when the instruction can write them.
//
//   PSPOCKET_FUZZ_SEED=<n>   seed of the random contexts (default 0x5EED); encodings are fixed at build time
//   PSPOCKET_FUZZ_ITERS=<n>  contexts per case (default 1)

#include <catch_amalgamated.hpp>

#include <cstdlib>
#include <map>
#include <vector>

#include "fuzz_case.h"
#include "pspocket/core/log.hpp"
#include "pspocket/core/reservation.h"
#include "pspocket/spu/disasm.hpp"
#include "spu_test_util.hpp"

using namespace pspocket;
using namespace pspocket::spu;
using namespace pspocket::spu::test;

namespace {

// The committed guest region: the DMA window plus room for a 16 KB transfer that starts at its end.
constexpr u32 kGuestCommit = PS_SPU_FUZZ_GUEST_SIZE + 0x8000;

u64 g_time = 0;
u64 fixed_time() { return g_time; }

u64 env_u64(const char* name, u64 fallback)
{
    const char* v = std::getenv(name);
    return v ? std::strtoull(v, nullptr, 0) : fallback;
}

// Interesting 32-bit words: floats at the edges of the SPU model, small integers, masks.
u32 random_word(Rng& rng)
{
    static constexpr u32 kSpecial[] = {0x00000000, 0x80000000, 0x00000001, 0x807FFFFF, 0x00800000, 0x80800000,
        0x3F800000, 0xBF800000, 0x3F000000, 0x40000000, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7F800001,
        0x7FC00000, 0xFFC00000, 0x7FFFFFFF, 0xFFFFFFFF, 0x4F000000, 0xCF000000, 0x4F800000, 0x4EFFFFFF, 0x33A00000,
        0x3F800001, 0x3F7FFFFF, 0x00FFFFFF, 0x7E800000, 0x01000000, 0x0000FFFF, 0xFFFF0000, 0x00008000, 0x7FFF7FFF};
    switch (rng.below(8)) {
    case 0:
    case 1: return kSpecial[rng.below(std::size(kSpecial))];
    case 2: return rng.below(64);
    case 3: return static_cast<u32>(-static_cast<s32>(rng.below(64)));
    case 4: return std::bit_cast<u32>(static_cast<float>(static_cast<s32>(rng.next() >> 40)) / (1 + rng.below(100)));
    case 5: return (static_cast<u32>(rng.next()) & 0x807FFFFFu) | ((126 + rng.below(4)) << 23);
    default: return static_cast<u32>(rng.next());
    }
}

u64 random_double(Rng& rng)
{
    static constexpr u64 kSpecial[] = {0, 0x8000000000000000ull, 1, 0x000FFFFFFFFFFFFFull, 0x0010000000000000ull,
        0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
        0x7FF8000000000000ull, 0x7FF0000000000001ull, 0x3CA0000000000000ull, 0x3FF0000000000001ull, 0x47EFFFFFE0000000ull,
        0x380FFFFFFFFFFFFFull, 0x3690000000000000ull};
    switch (rng.below(4)) {
    case 0: return kSpecial[rng.below(std::size(kSpecial))];
    case 1: return std::bit_cast<u64>(static_cast<double>(static_cast<s64>(rng.next())) / static_cast<double>(1 + rng.below(1000)));
    case 2: return (rng.next() & 0x800FFFFFFFFFFFFFull) | (u64{1018 + rng.below(12)} << 52);
    default: return rng.next();
    }
}

void random_context(SPUContext& ctx, Rng& rng)
{
    for (ps_spu_v128& r : ctx.gpr) {
        if (rng.chance(25)) {
            r.u64[0] = random_double(rng);
            r.u64[1] = random_double(rng);
        } else {
            for (u32& w : r.u32) w = random_word(rng);
        }
    }
    const ps_spu_v128 fpscr = words(static_cast<u32>(rng.next()), static_cast<u32>(rng.next()), static_cast<u32>(rng.next()),
        static_cast<u32>(rng.next()));
    ps_spu_fscrwr(&ctx, &fpscr);
    ctx.srr0 = static_cast<u32>(rng.next()) & PS_SPU_PC_MASK;
    ctx.interrupts_enabled = rng.chance(30) ? 1 : 0;

    ps_spu_channels& ch = ctx.ch;
    ch.mfc_lsa = static_cast<u32>(rng.next());
    ch.mfc_eah = rng.chance(80) ? 0 : static_cast<u32>(rng.next());
    ch.mfc_eal = static_cast<u32>(rng.next());
    ch.mfc_size = rng.below(0x10000);
    ch.mfc_tag = rng.below(32);
    ch.tag_mask = rng.chance(50) ? static_cast<u32>(rng.next()) : 1u << rng.below(32);
    ch.tag_update = rng.chance(70) ? 0 : 1 + rng.below(2);
    ch.tag_stat = static_cast<u32>(rng.next());
    ch.tag_stat_count = rng.below(2);
    ch.stall_stat = static_cast<u32>(rng.next());
    ch.stall_stat_count = rng.below(2);
    ch.atomic_stat = rng.below(8);
    ch.atomic_stat_count = rng.below(2);
    ch.in_mbox_count = rng.below(5);
    for (u32 i = 0; i < ch.in_mbox_count; i++) ch.in_mbox[i] = static_cast<u32>(rng.next());
    ch.out_mbox = static_cast<u32>(rng.next());
    ch.out_mbox_count = rng.below(2);
    ch.out_intr_mbox = static_cast<u32>(rng.next());
    ch.out_intr_mbox_count = rng.below(2);
    for (u32 i = 0; i < 2; i++) {
        ch.snr_count[i] = rng.below(2);
        ch.snr[i] = ch.snr_count[i] ? static_cast<u32>(rng.next()) : 0;
    }
    ch.snr_or_mode = rng.below(4);
    ch.event_mask = static_cast<u32>(rng.next()) & (rng.chance(50) ? 0x1FFFu : 0x0732u);
    ch.event_stat = static_cast<u32>(rng.next()) & 0x1FFFu;
    ch.event_count = rng.below(2);
    ch.dec_value = rng.chance(50) ? rng.below(1000) : static_cast<u32>(rng.next());
    ch.dec_frozen = rng.chance(20) ? 1 : 0;
    ch.dec_start = g_time - (rng.chance(50) ? rng.below(2000) : (rng.next() >> 28));
    ch.res_valid = 0;
}

void write_list(u8* ls, u32 at, u64 value2)
{
    Rng rng(value2 >> 8);
    const u32 elements = static_cast<u32>(value2 & 0xFF);
    static constexpr u32 kSizes[] = {0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 1024};
    for (u32 k = 0; k < elements; k++) {
        const u32 size = kSizes[rng.below(std::size(kSizes))];
        u32 ea = PS_SPU_FUZZ_GUEST_BASE + rng.below(PS_SPU_FUZZ_GUEST_SIZE - 1024);
        ea &= size >= 16 ? ~15u : ~(size ? size - 1 : 0u);
        const u32 stall = rng.chance(15) ? 0x80000000u : 0;
        ps_st32(ls + ((at + k * 8) & PS_SPU_LS_MASK), stall | size);
        ps_st32(ls + ((at + k * 8 + 4) & PS_SPU_LS_MASK), ea);
    }
}

void apply_overrides(SPUContext& ctx, const ps_spu_fuzz_case& c)
{
    for (u32 k = 0; k < c.override_count; k++) {
        const ps_spu_fuzz_override& o = ps_spu_fuzz_overrides[c.first_override + k];
        switch (o.kind) {
        case PS_SPU_FZ_PREF: sw(ctx.gpr[o.index], 0, static_cast<u32>(o.value)); break;
        case PS_SPU_FZ_REG:
            ctx.gpr[o.index].u64[0] = o.value;
            ctx.gpr[o.index].u64[1] = o.value2;
            break;
        case PS_SPU_FZ_MFC:
            ctx.ch.mfc_lsa = static_cast<u32>(o.value);
            ctx.ch.mfc_eal = static_cast<u32>(o.value >> 32);
            ctx.ch.mfc_eah = 0;
            ctx.ch.mfc_size = static_cast<u32>(o.value2 & 0xFFFF);
            ctx.ch.mfc_tag = static_cast<u32>(o.value2 >> 16) & 0x1F;
            break;
        case PS_SPU_FZ_RES: {
            const u32 addr = static_cast<u32>(o.value);
            if (o.index == 0) {
                ctx.ch.res_valid = 0;
                break;
            }
            ctx.ch.res_valid = 1;
            ctx.ch.res_addr = addr;
            ctx.ch.res_version = ps_res_acquire(ctx.membase, addr, ctx.ch.res_data);
            if (o.index == 2) ctx.ch.res_data[17] ^= 0x40;
            if (o.index == 3) ctx.ch.res_version += 2;
            break;
        }
        case PS_SPU_FZ_LIST: write_list(ctx.ls, static_cast<u32>(o.value), o.value2); break;
        default: FAIL("unknown override kind");
        }
    }
}

bool may_write_memory(const Inst& inst)
{
    const RegUse use = reg_use(inst);
    return use.ls_write || inst.op == Op::WRCH;
}

struct HookGuard {
    log::Level level = log::min_level();
    HookGuard()
    {
        ps_spu_set_timebase_source(fixed_time);
        log::set_min_level(log::Level::Fatal); // halts and exceptions log at Error
    }
    ~HookGuard()
    {
        ps_spu_set_timebase_source(nullptr);
        log::set_min_level(level);
    }
};

} // namespace

TEST_CASE("every supported instruction has at least 200 fuzz cases per floating-point model")
{
    std::map<std::pair<u32, u32>, u32> counts;
    for (u32 i = 0; i < ps_spu_fuzz_case_count; i++) counts[{ps_spu_fuzz_cases[i].op, ps_spu_fuzz_cases[i].xfloat}]++;

    const std::vector<Op> ops = supported_ops();
    CHECK(ops.size() == 194);
    CHECK(ps_spu_fuzz_cases_per_op >= 200);
    usize expected_keys = 0;
    for (const Op op : ops) {
        INFO("op " << op_info(op).mnemonic);
        CHECK(counts[{static_cast<u32>(op), PS_SPU_XFLOAT_APPROXIMATE}] >= 200);
        expected_keys++;
        if (op_info(op).flags & OPF_XFLOAT) {
            CHECK(counts[{static_cast<u32>(op), PS_SPU_XFLOAT_ACCURATE}] >= 200);
            expected_keys++;
        }
    }
    // ... and nothing else: every case belongs to a supported op and model.
    CHECK(counts.size() == expected_keys);

    // A quarter of them run relocated (D-081), away from where they were lifted.
    std::map<u32, u32> relocated;
    for (u32 i = 0; i < ps_spu_fuzz_case_count; i++)
        if (ps_spu_fuzz_cases[i].reloc) relocated[ps_spu_fuzz_cases[i].op]++;
    for (const Op op : ops) {
        INFO("op " << op_info(op).mnemonic);
        CHECK(relocated[static_cast<u32>(op)] >= 40);
    }
}

TEST_CASE("differential fuzz: interpreter and lifted code agree on every case")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    REQUIRE(mem.commit(PS_SPU_FUZZ_GUEST_BASE, kGuestCommit));
    LocalStore ls_init, ls_a, ls_b;
    Rng init(0x1234);
    for (u32 i = 0; i < PS_SPU_LS_SIZE; i += 8) ps_st64(ls_init.data() + i, init.next());
    std::vector<u8> guest_init(kGuestCommit), after_interp(kGuestCommit);
    for (u32 i = 0; i < kGuestCommit; i += 8) ps_st64(guest_init.data() + i, init.next());
    std::memcpy(ls_a.data(), ls_init.data(), PS_SPU_LS_SIZE);
    std::memcpy(ls_b.data(), ls_init.data(), PS_SPU_LS_SIZE);

    HookGuard hooks;
    FpEnv fp;
    const u64 seed = env_u64("PSPOCKET_FUZZ_SEED", 0x5EED);
    const u64 iterations = env_u64("PSPOCKET_FUZZ_ITERS", 1);
    u64 failures = 0, executed = 0;

    for (u64 it = 0; it < iterations; it++) {
        for (u32 i = 0; i < ps_spu_fuzz_case_count; i++) {
            const ps_spu_fuzz_case& c = ps_spu_fuzz_cases[i];
            const Inst inst = decode(c.insn);
            const bool memory = may_write_memory(inst);
            Rng rng(seed * 0x2545F4914F6CDD1Dull + i + (it << 40));
            g_time = 0x100000000ull + (rng.next() >> 24);

            SPUContext start{};
            ps_spu_context_init(&start, ls_a.data(), mem.base());
            random_context(start, rng);
            start.xfloat = c.xfloat;
            const u32 pc = (c.pc + c.reloc) & PS_SPU_LS_MASK; // where it runs
            start.pc = pc;
            start.reloc = c.reloc;

            // interpreter
            if (memory) mem.write_bytes(PS_SPU_FUZZ_GUEST_BASE, guest_init.data(), kGuestCommit);
            SPUContext interp = start;
            apply_overrides(interp, c);
            const u32 saved_word = ps_ld32(ls_a.data() + pc);
            ps_st32(ls_a.data() + pc, c.insn);
            step(interp);
            if (memory) mem.read_bytes(PS_SPU_FUZZ_GUEST_BASE, after_interp.data(), kGuestCommit);

            // lifted
            if (memory) mem.write_bytes(PS_SPU_FUZZ_GUEST_BASE, guest_init.data(), kGuestCommit);
            SPUContext lifted = start;
            lifted.ls = ls_b.data();
            apply_overrides(lifted, c);
            ps_st32(ls_b.data() + pc, c.insn);
            c.fn(&lifted, ls_b.data());
            executed++;

            lifted.ls = interp.ls;
            interp.ch.res_version = lifted.ch.res_version = 0; // global version counters move between the two runs
            std::string diff = diff_contexts(interp, lifted);
            if (memory) {
                if (std::memcmp(ls_a.data(), ls_b.data(), PS_SPU_LS_SIZE) != 0) {
                    for (u32 b = 0; b < PS_SPU_LS_SIZE; b++)
                        if (ls_a.data()[b] != ls_b.data()[b]) {
                            diff += std::format(" ls[{:#x}]: {:#04x} vs {:#04x};", b, ls_a.data()[b], ls_b.data()[b]);
                            break;
                        }
                }
                for (u32 b = 0; b < kGuestCommit; b++)
                    if (after_interp[b] != mem.base()[PS_SPU_FUZZ_GUEST_BASE + b]) {
                        diff += std::format(" guest[{:#x}] differs;", PS_SPU_FUZZ_GUEST_BASE + b);
                        break;
                    }
                std::memcpy(ls_a.data(), ls_init.data(), PS_SPU_LS_SIZE);
                std::memcpy(ls_b.data(), ls_init.data(), PS_SPU_LS_SIZE);
            } else {
                ps_st32(ls_a.data() + pc, saved_word);
                ps_st32(ls_b.data() + pc, saved_word);
            }
            if (!diff.empty() && ++failures <= 25) {
                UNSCOPED_INFO(std::format("case {} ({}{}) {:08x} '{}' at {:#07x} (lifted at {:#07x}), seed {:#x} iteration {}:{}",
                    i, op_info(static_cast<Op>(c.op)).mnemonic, c.xfloat ? ", accurate" : "", c.insn,
                    disassemble(c.insn, pc), pc, c.pc, seed, it, diff));
                CHECK(diff.empty());
            }
        }
    }
    INFO(failures << " mismatches in " << executed << " executions");
    CHECK(failures == 0);
    CHECK(executed == u64{ps_spu_fuzz_case_count} * iterations);
}

TEST_CASE("program-level fuzz: a lifted program agrees with the interpreter wherever it runs (D-081)")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    ps_spu_fzp_register();
    ps_spu_fzq_register();
    HookGuard hooks;
    // Its absolute call leaves the program for the stub: a miss, interpreted (CLAUDE.md rule 5 aborts dev builds).
    const ps_spu_miss_policy policy = ps_spu_get_miss_policy();
    ps_spu_set_miss_policy(PS_SPU_MISS_INTERPRET);

    struct Placement {
        const ps_spu_program* program;
        u32 reloc;
    };
    const Placement placements[] = {
        {&ps_spu_fzq_program, 0},       // fixed, where it was lifted
        {&ps_spu_fzp_program, 0},       // relocatable, where it was lifted
        {&ps_spu_fzp_program, 0x2340},  // higher
        {&ps_spu_fzp_program, 0x3F800}, // lower (0x800 below)
        {&ps_spu_fzp_program, 0x1234},  // not quadword aligned: its relative loads read other quadwords
        {&ps_spu_fzp_program, 0x3EFE0}, // across the end of local store
    };
    LocalStore ls_interp, ls_lifted;
    for (const auto& [program, reloc] : placements) {
        INFO(std::format("{} at reloc {:#x}", program->name, reloc));
        const u32 at = (PS_SPU_FUZZ_PROGRAM_BASE + reloc) & PS_SPU_LS_MASK;
        Rng fill(0x51DE);
        for (u32 i = 0; i < PS_SPU_LS_SIZE; i += 8) ps_st64(ls_interp.data() + i, fill.next());
        for (u32 i = 0; i < ps_spu_fuzz_program_size; i++)
            ls_interp.data()[(at + i) & PS_SPU_LS_MASK] = ps_spu_fuzz_program[i];
        ps_st32(ls_interp.data() + PS_SPU_FUZZ_PROGRAM_STUB, 0x35000000u); // bi $lr
        std::memcpy(ls_lifted.data(), ls_interp.data(), PS_SPU_LS_SIZE);

        SPUContext interp{}, lifted{};
        ps_spu_context_init(&interp, ls_interp.data(), mem.base());
        ps_spu_context_init(&lifted, ls_lifted.data(), mem.base());
        interp.pc = lifted.pc = (ps_spu_fuzz_program_entry + reloc) & PS_SPU_LS_MASK;
        ps_spu_attach_program_at(&lifted, program, at);
        const u64 misses = ps_spu_miss_count();

        for (const u32 code : {0x10u, 0x11u}) { // runs to the first stop, then resumes after it
            REQUIRE(ps_spu_interpret(&interp, 0) == PS_SPU_STOP_SIGNAL);
            REQUIRE(ps_spu_run(&lifted) == PS_SPU_STOP_SIGNAL);
            CHECK(interp.stop_code == code);
            CHECK(lifted.stop_code == code);
            SPUContext compared = lifted;
            compared.ls = interp.ls;
            compared.program = interp.program;
            compared.home = interp.home;
            compared.reloc = interp.reloc;
            compared.home_reloc = interp.home_reloc;
            CHECK(diff_contexts(interp, compared).empty());
            CHECK(std::memcmp(ls_interp.data(), ls_lifted.data(), PS_SPU_LS_SIZE) == 0);
        }
        CHECK(gw(lifted.gpr[4], 0) == 20); // 5 + 4 + 3 + 2 + 1, and func1 five times
        CHECK(gw(lifted.gpr[9], 0) == 0x77);
        CHECK(gw(lifted.gpr[8], 0) == 2);
        CHECK(gw(lifted.gpr[7], 0) == 0); // jumped over
        // Lifted code ran throughout: the one miss is the stub, outside the program.
        CHECK(lifted.program == program);
        CHECK(ps_spu_miss_count() - misses == 1);
    }

    ps_spu_set_miss_policy(policy);
    ps_spu_unregister_program(&ps_spu_fzp_program);
    ps_spu_unregister_program(&ps_spu_fzq_program);
}
