// Program-level tests: the synthetic test programs (recomp/ppu synthetic.hpp) run on the interpreter and as lifted
// code (generated at build time by ppu-lift into ppu_progs_*.c). Both runs start from the same randomised context;
// the final contexts, the data memory and the stack must match, and the results must match a native C++ reference.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "ppu_progs.h"
#include "ppu_test_util.hpp"
#include "pspocket/ppu/interpreter.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/ppu/synthetic.hpp"

using namespace pspocket;
using namespace pspocket::ppu;
namespace synthetic = pspocket::recomp::ppu::synthetic;

namespace {

constexpr u32 kProgramsBase = 0x00100000; // tests/ppu/CMakeLists.txt passes the same --base to ppu-lift
constexpr u32 kData = 0x00200000;
constexpr u32 kDataSize = 0x00010000;
constexpr u32 kStackBase = 0x00E00000;
constexpr u32 kStackSize = 0x00100000;
constexpr u32 kStackTop = kStackBase + kStackSize - 0x1000;
constexpr u32 kSentinel = 0x00000100; // return address the runs stop at

enum class Mode { Interpreter, Lifted, InterpreterCallingLifted };

struct Env {
    GuestMemory mem;
    u8* base = nullptr;
    synthetic::Program prog = synthetic::build_test_programs(kProgramsBase);

    Env()
    {
        REQUIRE(test::reserve_for_lifted(mem));
        REQUIRE(mem.commit(prog.base, (static_cast<u32>(prog.bytes.size()) + 0xFFFF) & ~0xFFFFu));
        REQUIRE(mem.commit(kData, kDataSize));
        REQUIRE(mem.commit(kStackBase, kStackSize));
        base = mem.base();
        mem.write_bytes(prog.base, prog.bytes.data(), prog.bytes.size());
        ppu_progs_register();
    }
    ~Env() { ppu_progs_unregister(); }

    u32 sym(const char* name) const { return prog.symbol(name); }
};

Env& env()
{
    static Env e;
    return e;
}

u64 g_syscall_count = 0;
void test_syscall(PPUContext* ctx, u8*)
{
    g_syscall_count++;
    ctx->gpr[3] = ctx->gpr[3] * 3 + ctx->gpr[11];
}
void test_trap(PPUContext* ctx, u8*, u32) { ctx->gpr[3] = 1000; }

struct Hooks {
    Hooks()
    {
        ps_ppu_set_syscall_handler(test_syscall);
        ps_ppu_set_trap_handler(test_trap);
    }
    ~Hooks()
    {
        ps_ppu_set_syscall_handler(nullptr);
        ps_ppu_set_trap_handler(nullptr);
    }
};

// A deterministic, register-rich starting context: everything random except the ABI registers we set.
PPUContext start_context(u64 seed)
{
    PPUContext ctx;
    test::Rng rng(seed);
    test::random_context(ctx, rng);
    ctx.reserve_valid = 0;
    ctx.fpscr = 0;
    ctx.gpr[1] = kStackTop;
    ctx.gpr[2] = env().prog.toc;
    ctx.lr = kSentinel;
    return ctx;
}

struct Run {
    PPUContext ctx;
    std::vector<u8> data;
    std::vector<u8> stack;
    u64 steps = 0;
    u64 lifted_calls = 0;
};

// Runs `entry` from ctx with the data area initialised to `data`.
Run run(Mode mode, u32 entry, PPUContext ctx, const std::vector<u8>& data)
{
    Env& e = env();
    e.mem.write_bytes(kData, data.data(), data.size());
    std::vector<u8> zero(kStackSize, 0);
    e.mem.write_bytes(kStackBase, zero.data(), zero.size());
    Run r;
    if (mode == Mode::Lifted) {
        const ps_ppu_fn fn = ps_ppu_dispatch_find(entry);
        REQUIRE(fn != nullptr);
        fn(&ctx, e.base);
    } else {
        RunOptions opt;
        opt.stop_at_return = true;
        opt.stop_lr = kSentinel;
        opt.stop_sp = ctx.gpr[1];
        opt.call_lifted = mode == Mode::InterpreterCallingLifted;
        opt.max_steps = 500'000'000;
        ctx.cia = entry;
        const RunResult res = ppu::run(ctx, e.base, opt);
        REQUIRE(res.reason == StopReason::Returned);
        r.steps = res.steps;
        r.lifted_calls = res.lifted_calls;
    }
    r.ctx = ctx;
    r.data.resize(data.size());
    e.mem.read_bytes(kData, r.data.data(), r.data.size());
    r.stack.resize(kStackSize);
    e.mem.read_bytes(kStackBase, r.stack.data(), r.stack.size());
    return r;
}

// Runs on the interpreter and lifted; checks they agree completely. Returns the interpreter run.
Run both(u32 entry, const PPUContext& ctx, const std::vector<u8>& data = std::vector<u8>(256, 0))
{
    const Run a = run(Mode::Interpreter, entry, ctx, data);
    const Run b = run(Mode::Lifted, entry, ctx, data);
    const std::string diff = test::diff_contexts(a.ctx, b.ctx, false, /*ignore_cia*/ true);
    INFO("entry " << std::hex << entry << " context diff:" << diff);
    CHECK(diff.empty());
    CHECK(a.data == b.data);
    CHECK(a.stack == b.stack);
    return a;
}

void put32(std::vector<u8>& v, usize off, u32 x)
{
    v[off] = static_cast<u8>(x >> 24);
    v[off + 1] = static_cast<u8>(x >> 16);
    v[off + 2] = static_cast<u8>(x >> 8);
    v[off + 3] = static_cast<u8>(x);
}
u32 get32(const std::vector<u8>& v, usize off)
{
    return (u32{v[off]} << 24) | (u32{v[off + 1]} << 16) | (u32{v[off + 2]} << 8) | v[off + 3];
}
void put64(std::vector<u8>& v, usize off, u64 x)
{
    put32(v, off, static_cast<u32>(x >> 32));
    put32(v, off + 4, static_cast<u32>(x));
}
u64 get64(const std::vector<u8>& v, usize off) { return (u64{get32(v, off)} << 32) | get32(v, off + 4); }

} // namespace

TEST_CASE("sum_to: loop with mtctr/bdnz")
{
    for (u64 n : {0ull, 1ull, 10ull, 1000ull}) {
        PPUContext c = start_context(n);
        c.gpr[3] = n;
        CHECK(both(env().sym("sum_to"), c).ctx.gpr[3] == n * (n + 1) / 2);
    }
}

TEST_CASE("fib: recursion, frames, LR save/restore")
{
    for (u32 n : {0u, 1u, 2u, 10u, 20u}) {
        PPUContext c = start_context(100 + n);
        c.gpr[3] = n;
        CHECK(both(env().sym("fib"), c).ctx.gpr[3] == synthetic::fib_reference(n));
    }
}

TEST_CASE("memcpy / memset / copy_dwords")
{
    std::vector<u8> data(0x4000);
    for (usize i = 0; i < 0x1000; i++) data[i] = static_cast<u8>(i * 7 + 3);
    for (u64 n : {0ull, 1ull, 100ull, 1000ull}) {
        PPUContext c = start_context(200 + n);
        c.gpr[3] = kData + 0x2000;
        c.gpr[4] = kData;
        c.gpr[5] = n;
        const Run r = both(env().sym("memcpy_bytes"), c, data);
        CHECK(r.ctx.gpr[3] == kData + 0x2000);
        CHECK(std::equal(r.data.begin() + 0x2000, r.data.begin() + 0x2000 + static_cast<long>(n), data.begin()));
        CHECK(r.data[0x2000 + n] == 0);
    }
    PPUContext c = start_context(300);
    c.gpr[3] = kData + 0x100;
    c.gpr[4] = 0xFFFFFFAB; // only the low byte is stored
    c.gpr[5] = 333;
    Run r = both(env().sym("memset_bytes"), c, data);
    CHECK(std::all_of(r.data.begin() + 0x100, r.data.begin() + 0x100 + 333, [](u8 b) { return b == 0xAB; }));
    CHECK(r.data[0x100 + 333] == data[0x100 + 333]);

    c = start_context(301);
    c.gpr[3] = kData + 0x3000;
    c.gpr[4] = kData + 8;
    c.gpr[5] = 50;
    r = both(env().sym("copy_dwords"), c, data);
    CHECK(std::equal(r.data.begin() + 0x3000, r.data.begin() + 0x3000 + 400, data.begin() + 8));
}

TEST_CASE("bubble_sort sorts signed words")
{
    test::Rng rng(7);
    std::vector<u8> data(1024);
    std::vector<s32> native(64);
    for (usize i = 0; i < native.size(); i++) {
        native[i] = static_cast<s32>(rng.next());
        put32(data, i * 4, static_cast<u32>(native[i]));
    }
    std::sort(native.begin(), native.end());
    PPUContext c = start_context(400);
    c.gpr[3] = kData;
    c.gpr[4] = native.size();
    const Run r = both(env().sym("bubble_sort"), c, data);
    for (usize i = 0; i < native.size(); i++) CHECK(static_cast<s32>(get32(r.data, i * 4)) == native[i]);
}

TEST_CASE("switches through recognised jump tables")
{
    auto sw_rel = [](u64 x) -> u64 {
        switch (static_cast<u32>(x)) {
        case 0: return 100;
        case 1: return 111;
        case 2: return 122;
        case 3: return x + 1000;
        case 4: return 122;
        case 5: return 155;
        case 6: return static_cast<u64>(-66);
        default: return ~0ull;
        }
    };
    for (u64 x : {0ull, 1ull, 2ull, 3ull, 4ull, 5ull, 6ull, 7ull, 100ull, 0xFFFFFFFFull, 0x100000003ull}) {
        PPUContext c = start_context(500 + x);
        c.gpr[3] = x;
        CHECK(both(env().sym("sw_rel"), c).ctx.gpr[3] == sw_rel(x));
    }
    auto sw_toc = [](u64 x) -> u64 {
        switch (static_cast<u32>(x)) {
        case 0: return 10;
        case 1: return 20;
        case 2: return x * 7;
        case 3: return 40;
        case 4: return 0 - x;
        default: return 0;
        }
    };
    for (u64 x = 0; x < 7; x++) {
        PPUContext c = start_context(600 + x);
        c.gpr[3] = x;
        CHECK(both(env().sym("sw_toc"), c).ctx.gpr[3] == sw_toc(x));
    }
    auto sw_abs = [](u64 x) -> u64 {
        switch (static_cast<u32>(x)) {
        case 0: return 1000;
        case 1: return 2000;
        case 2: return 50 - x;
        case 3: return 4000;
        default: return 9;
        }
    };
    for (u64 x = 0; x < 6; x++) {
        PPUContext c = start_context(700 + x);
        c.gpr[3] = x;
        CHECK(both(env().sym("sw_abs"), c).ctx.gpr[3] == sw_abs(x));
    }
}

TEST_CASE("function pointers through bctrl and the dispatch table")
{
    PPUContext c = start_context(800);
    c.gpr[3] = env().sym("add_one");
    c.gpr[4] = 41;
    CHECK(both(env().sym("apply"), c).ctx.gpr[3] == 42);
    c.gpr[3] = env().sym("times_three");
    c.gpr[4] = 5;
    CHECK(both(env().sym("apply"), c).ctx.gpr[3] == 15);
    for (u64 i = 0; i < 2; i++) {
        c = start_context(810 + i);
        c.gpr[3] = i;
        c.gpr[4] = 7;
        CHECK(both(env().sym("apply_table"), c).ctx.gpr[3] == (i == 0 ? 8u : 21u));
    }
}

TEST_CASE("interop: a dispatch miss runs the interpreter until the call returns (release policy)")
{
    const ps_ppu_miss_policy saved = ps_ppu_get_miss_policy();
    ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
    const u64 misses = ps_ppu_dispatch_miss_count();

    // secret_fn was never seeded: lifted apply_table reaches it through bctrl and misses.
    PPUContext c = start_context(900);
    c.gpr[3] = 2;
    c.gpr[4] = 0x1234;
    CHECK(both(env().sym("apply_table"), c).ctx.gpr[3] == (0x1234u ^ 0x5a5au));
    CHECK(ps_ppu_dispatch_miss_count() == misses + 1);

    // A lifted function removed from the table is interpreted instead, with identical results.
    ps_ppu_dispatch_unregister(env().sym("add_one"));
    c = start_context(901);
    c.gpr[3] = env().sym("add_one");
    c.gpr[4] = 99;
    CHECK(both(env().sym("apply"), c).ctx.gpr[3] == 100);
    CHECK(ps_ppu_dispatch_miss_count() == misses + 2);
    ppu_progs_register();
    ps_ppu_set_miss_policy(saved);
}

TEST_CASE("interop: the interpreter calls lifted functions and continues at LR")
{
    for (u64 n : {3ull, 10ull, 15ull}) {
        PPUContext c = start_context(1000 + n);
        c.gpr[3] = n;
        const std::vector<u8> data(256, 0);
        const Run interp = run(Mode::Interpreter, env().sym("driver"), c, data);
        const Run mixed = run(Mode::InterpreterCallingLifted, env().sym("driver"), c, data);
        CHECK(mixed.lifted_calls == 3);
        CHECK(mixed.steps < interp.steps);
        CHECK(test::diff_contexts(interp.ctx, mixed.ctx, false, true).empty());
        CHECK(interp.stack == mixed.stack);
        const u64 expected = n * (n + 1) / 2 + synthetic::fib_reference(static_cast<u32>(n)) +
            (std::array<u64, 8>{100, 111, 122, n + 1000, 122, 155, static_cast<u64>(-66), ~0ull}[n & 7]);
        CHECK(interp.ctx.gpr[3] == expected);
    }
}

TEST_CASE("floating point programs")
{
    test::Rng rng(11);
    std::vector<u8> data(1024);
    std::vector<double> a(16), b(16);
    double acc = 0.0;
    for (int i = 0; i < 16; i++) {
        a[i] = (static_cast<double>(rng.below(2000)) - 1000.0) / 7.0;
        b[i] = (static_cast<double>(rng.below(2000)) - 1000.0) / 3.0;
        put64(data, static_cast<usize>(i) * 8, std::bit_cast<u64>(a[i]));
        put64(data, 128 + static_cast<usize>(i) * 8, std::bit_cast<u64>(b[i]));
        acc = std::fma(a[i], b[i], acc);
    }
    PPUContext c = start_context(1100);
    c.gpr[3] = kData;
    c.gpr[4] = kData + 128;
    c.gpr[5] = 16;
    Run r = both(env().sym("dot_sqrt"), c, data);
    CHECK(std::bit_cast<u64>(r.ctx.fpr[1]) == std::bit_cast<u64>(std::sqrt(acc)));

    std::vector<float> fl = {1.5f, 2.25f, -0.125f, 1e7f, 3.0f, -2.5f, 0.1f, 7.0f};
    double facc = 0.0;
    for (usize i = 0; i < fl.size(); i++) {
        put32(data, 256 + i * 4, std::bit_cast<u32>(fl[i]));
        facc = static_cast<double>(static_cast<float>(facc + static_cast<double>(fl[i])));
    }
    c = start_context(1101);
    c.gpr[3] = kData + 256;
    c.gpr[4] = fl.size();
    r = both(env().sym("sum_floats_to_int"), c, data);
    CHECK(r.ctx.gpr[3] == static_cast<u64>(static_cast<s64>(static_cast<s32>(facc))));

    const double pairs[][2] = {{1.0, 2.0}, {3.0, -1.0}, {std::nan(""), 1.0}, {1.0, std::nan("")}, {-0.0, 0.0}};
    for (const auto& p : pairs) {
        c = start_context(1102);
        c.fpr[1] = p[0];
        c.fpr[2] = p[1];
        r = both(env().sym("fmax"), c);
        const double expected = !(p[0] < p[1]) ? p[0] : p[1];
        CHECK(std::bit_cast<u64>(r.ctx.fpr[1]) == std::bit_cast<u64>(expected));
    }
}

TEST_CASE("256-bit add/sub carry chains (addc/adde/addze, subfc/subfe)")
{
    test::Rng rng(12);
    for (int iter = 0; iter < 8; iter++) {
        std::vector<u8> data(256);
        u64 x[4], y[4];
        for (int i = 0; i < 4; i++) {
            x[i] = iter == 0 ? ~0ull : rng.next();
            y[i] = iter == 0 ? (i == 0 ? 1 : 0) : rng.next();
            put64(data, 32 + static_cast<usize>(i) * 8, x[i]);
            put64(data, 64 + static_cast<usize>(i) * 8, y[i]);
        }
        PPUContext c = start_context(1200 + static_cast<u64>(iter));
        c.gpr[3] = kData;
        c.gpr[4] = kData + 32;
        c.gpr[5] = kData + 64;
        Run r = both(env().sym("add256"), c, data);
        unsigned __int128 carry = 0;
        for (int i = 0; i < 4; i++) {
            const unsigned __int128 s = static_cast<unsigned __int128>(x[i]) + y[i] + carry;
            CHECK(get64(r.data, static_cast<usize>(i) * 8) == static_cast<u64>(s));
            carry = s >> 64;
        }
        CHECK(r.ctx.gpr[3] == static_cast<u64>(carry));

        r = both(env().sym("sub256"), c, data);
        u64 borrow = 0;
        for (int i = 0; i < 4; i++) {
            const u64 diff = x[i] - y[i] - borrow;
            const u64 next_borrow = (x[i] < y[i]) || (x[i] - y[i] < borrow) ? 1 : 0;
            CHECK(get64(r.data, static_cast<usize>(i) * 8) == diff);
            borrow = next_borrow;
        }
        CHECK(r.ctx.gpr[3] == (borrow ? ~0ull : 0));
    }
}

TEST_CASE("atomics, syscalls, traps, PC-relative code, string moves, CR games, tail calls")
{
    Hooks hooks;
    std::vector<u8> data(256);
    put32(data, 16, 1000);
    PPUContext c = start_context(1300);
    c.gpr[3] = kData + 16;
    c.gpr[4] = 5;
    Run r = both(env().sym("atomic_add"), c, data);
    CHECK(r.ctx.gpr[3] == 1005);
    CHECK(get32(r.data, 16) == 1005);

    g_syscall_count = 0;
    c = start_context(1301);
    c.gpr[3] = 10;
    r = both(env().sym("syscall_test"), c);
    CHECK(r.ctx.gpr[3] == 10 * 3 + 77);
    CHECK(g_syscall_count == 2); // interpreter + lifted

    c = start_context(1302);
    c.gpr[3] = 13;
    CHECK(both(env().sym("trap_test"), c).ctx.gpr[3] == 1001);
    c.gpr[3] = 5;
    CHECK(both(env().sym("trap_test"), c).ctx.gpr[3] == 6);

    c = start_context(1303);
    CHECK(both(env().sym("pc_rel"), c).ctx.gpr[3] == 0xC0FFEE11);

    for (usize i = 0; i < 32; i++) data[64 + i] = static_cast<u8>(0xA0 + i);
    c = start_context(1304);
    c.gpr[3] = kData + 128;
    c.gpr[4] = kData + 64;
    r = both(env().sym("str_copy13"), c, data);
    CHECK(std::equal(r.data.begin() + 128, r.data.begin() + 141, data.begin() + 64));
    CHECK(r.data[141] == 0);

    for (auto [x, y] : std::vector<std::pair<u64, u64>>{{1, 2}, {5, 5}, {0xFFFFFFFF, 0}, {7, 0xFFFFFFFE}}) {
        c = start_context(1305 + x);
        c.gpr[3] = x;
        c.gpr[4] = y;
        u8 cr[32];
        std::copy(std::begin(c.cr), std::end(c.cr), cr);
        const s32 sx = static_cast<s32>(x), sy = static_cast<s32>(y);
        const u32 ux = static_cast<u32>(x), uy = static_cast<u32>(y);
        cr[0] = sx < sy; cr[1] = sx > sy; cr[2] = sx == sy; cr[3] = c.xer_so;
        cr[4] = sx < 0; cr[5] = sx > 0; cr[6] = sx == 0; cr[7] = c.xer_so;
        cr[28] = uy < ux; cr[29] = uy > ux; cr[30] = uy == ux; cr[31] = c.xer_so;
        cr[2] = cr[0] | cr[5];
        cr[6] = 0;
        for (int i = 0; i < 4; i++) cr[20 + i] = cr[28 + i];
        cr[20] = 1;
        cr[9] = !(cr[28] & cr[1]);
        u64 mfcr = 0;
        for (int i = 0; i < 32; i++) mfcr = (mfcr << 1) | cr[i];
        r = both(env().sym("cr_games"), c);
        CHECK(r.ctx.gpr[3] == mfcr + 0x50);
        CHECK(r.ctx.cr[25] == 1); // field 6 = 0x5 from mtcrf
        CHECK(r.ctx.cr[28] == 1); // field 7 = 0xA
    }

    c = start_context(1310);
    c.gpr[3] = 48;
    c.gpr[4] = 18;
    CHECK(both(env().sym("gcd"), c).ctx.gpr[3] == 6);
    c.gpr[3] = 17;
    c.gpr[4] = 5;
    CHECK(both(env().sym("gcd"), c).ctx.gpr[3] == 1);

    c = start_context(1311);
    c.gpr[3] = 5;
    CHECK(both(env().sym("tail_calls"), c).ctx.gpr[3] == 11);
    c.gpr[3] = 1;
    CHECK(both(env().sym("tail_calls"), c).ctx.gpr[3] == 18);

    c = start_context(1312);
    c.gpr[3] = 10;
    CHECK(both(env().sym("driver"), c).ctx.gpr[3] == 55 + 55 + 122);
}

TEST_CASE("rarer control flow: conditional bcctr, blrl, conditional bcl, bdzlr, trap resume, fall-through")
{
    Hooks hooks;
    PPUContext c = start_context(1400);
    c.gpr[3] = 9;
    c.gpr[4] = env().sym("add_one");
    CHECK(both(env().sym("cond_ctr"), c).ctx.gpr[3] == 10);
    c.gpr[3] = 0;
    CHECK(both(env().sym("cond_ctr"), c).ctx.gpr[3] == ~0ull);

    c = start_context(1401);
    c.gpr[3] = env().sym("times_three");
    c.gpr[4] = 14;
    CHECK(both(env().sym("blrl_call"), c).ctx.gpr[3] == 42);

    c = start_context(1402);
    c.gpr[3] = 5;
    CHECK(both(env().sym("cond_call"), c).ctx.gpr[3] == 6);
    c.gpr[3] = 4;
    CHECK(both(env().sym("cond_call"), c).ctx.gpr[3] == 4);

    for (u64 n : {1ull, 2ull, 17ull}) {
        c = start_context(1403 + n);
        c.gpr[3] = n;
        const Run r = both(env().sym("count_bdzlr"), c);
        CHECK(r.ctx.gpr[3] == n);
        CHECK(r.ctx.ctr == 0);
    }

    // The lifted trap resumes in the interpreter until the function returns (ps_ppu_interp_until).
    c = start_context(1410);
    c.gpr[3] = 1;
    CHECK(both(env().sym("trap_resume"), c).ctx.gpr[3] == 1007);

    c = start_context(1411);
    c.gpr[3] = 5;
    CHECK(both(env().sym("fall_a"), c).ctx.gpr[3] == 16);
}

TEST_CASE("a jump table patched at runtime takes the trapping default and continues in the interpreter (release)")
{
    const ps_ppu_miss_policy saved = ps_ppu_get_miss_policy();
    ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
    Env& e = env();
    // sw_abs's absolute table follows its bctr; point entry 0 at the default case (not in the lifted switch).
    const u32 sw = e.sym("sw_abs");
    u32 bctr = sw;
    while (ps_read32(e.base, bctr) != 0x4E800420u) bctr += 4;
    const u32 table = bctr + 4;
    const u32 original = ps_read32(e.base, table);
    const u32 default_case = ps_read32(e.base, table + 12) + 8; // entry 3's code is "li r3,4000; blr", default follows
    REQUIRE(ps_read32(e.base, default_case) == 0x38600009u);   // li r3,9
    ps_write32(e.base, table, default_case);
    const u64 misses = ps_ppu_dispatch_miss_count();

    PPUContext c = start_context(1500);
    c.gpr[3] = 0;
    CHECK(both(sw, c).ctx.gpr[3] == 9);
    CHECK(ps_ppu_dispatch_miss_count() > misses);

    ps_write32(e.base, table, original);
    ps_ppu_set_miss_policy(saved);
}
