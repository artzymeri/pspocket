// Native replacements of SPU middleware functions (D-965): the NEON building blocks against the lifted code's helpers,
// and the derived programs a known build gets (dispatch to native code, the lifted function when it declines).
// Synthetic programs only (CLAUDE.md rule 1).

#include <catch_amalgamated.hpp>

#include <cstring>
#include <string>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/spu/replace.hpp"
#include "pspocket/spu/runtime.h"
#include "spu_test_util.hpp"

extern "C" {
unsigned ps_test_replace_frest(void);
unsigned ps_test_replace_frsqest(void);
unsigned ps_test_replace_arith(uint32_t rounds);
unsigned ps_test_replace_ld4(void);
}

using namespace pspocket;
namespace rp = pspocket::spu::replace;

TEST_CASE("SPU replace: NEON building blocks give what the lifted code's helpers give (D-965)", "[spu]")
{
    const spu::test::FpEnv fp;
    CHECK(ps_test_replace_frest() == 0);
    CHECK(ps_test_replace_frsqest() == 0);
    CHECK(ps_test_replace_arith(20000) == 0);
    CHECK(ps_test_replace_ld4() == 0);
}

namespace {

// A program of two functions, hand-written: E at kStart, F at kStart + 0x10 ("lifted": $3 + 1, then return).
constexpr u32 kStart = 0x3000, kSize = 0x40;
constexpr u32 kAllCode[(kSize / 4 + 31) / 32] = {~0u};
void fn_e(SPUContext* ctx, uint8_t*) { ctx->pc = ctx->gpr[0].u32[3]; }
void fn_f(SPUContext* ctx, uint8_t*)
{
    ctx->gpr[3].u32[3] += 1;
    ctx->pc = ctx->gpr[0].u32[3];
}
// F natively: $3 + 100, and declines $3 = 0.
int native_f(SPUContext* ctx, uint8_t*)
{
    if (ctx->gpr[3].u32[3] == 0) return 0;
    ctx->gpr[3].u32[3] += 100;
    ctx->pc = ctx->gpr[0].u32[3];
    return 1;
}

struct TestProgram {
    std::vector<u8> image = std::vector<u8>(kSize);
    std::vector<ps_spu_entry> entries;
    ps_spu_program program{};
    explicit TestProgram(u32 seed)
    {
        for (u32 b = 0; b < kSize; b += 4) ps_st32(image.data() + b, seed * 0x01000193u ^ b);
        entries = {{kStart, fn_e}, {kStart + 0x10, fn_f}};
        const u64 hash = ps_spu_hash64(image.data(), kSize);
        program = ps_spu_program{"spu_replace_test", hash, kStart, kSize, PS_SPU_XFLOAT_APPROXIMATE, 1, 2, entries.data(),
            nullptr, kAllCode, ps_spu_code_hash(image.data(), kSize, kAllCode)};
    }
    ~TestProgram() { ps_spu_unregister_program(&program); }
};

u32 call(const ps_spu_program* p, u32 entry, u32 r3)
{
    SPUContext ctx{};
    ps_spu_attach_program_at(&ctx, p, kStart);
    ctx.gpr[3].u32[3] = r3;
    ctx.gpr[0].u32[3] = 0x100; // the return address
    ctx.pc = entry;
    const ps_spu_fn fn = ps_spu_lookup(&ctx, entry);
    REQUIRE(fn);
    fn(&ctx, nullptr);
    CHECK(ctx.pc == 0x100);
    return ctx.gpr[3].u32[3];
}

} // namespace

TEST_CASE("SPU replace: a known build is found as a program derived from it, its listed functions native", "[spu]")
{
    rp::set_mode(rp::Mode::on);
    TestProgram t(1);
    rp::add_build("replace test build", t.program.hash, kStart, kSize, {{kStart + 0x10, native_f, {}}});
    ps_spu_register_program(&t.program);
    const ps_spu_program* found = ps_spu_find_program(t.program.hash, kStart, kSize);
    REQUIRE(found);
    CHECK(found != &t.program);
    CHECK(rp::original_of(found) == &t.program);
    CHECK(rp::derived_of(&t.program) == found);
    CHECK(std::string(found->name) == t.program.name);
    // F runs natively; a declined call runs the lifted function; E (not listed) is the lifted function as it was.
    CHECK(call(found, kStart + 0x10, 5) == 105);
    CHECK(call(found, kStart + 0x10, 0) == 1);
    CHECK(call(found, kStart, 7) == 7);
    // The original, found by nothing now, still runs the lifted code.
    CHECK(call(&t.program, kStart + 0x10, 5) == 6);
    bool seen = false;
    for (const rp::FunctionStats& s : rp::stats())
        if (s.build == "replace test build") {
            seen = true;
            CHECK(s.native == 1);
            CHECK(s.declined == 1);
        }
    CHECK(seen);
    // Taking the original out takes the derived one out with it.
    ps_spu_unregister_program(&t.program);
    CHECK(ps_spu_find_program(t.program.hash, kStart, kSize) == nullptr);
}
