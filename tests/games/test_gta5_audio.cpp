// GTA V's audio job functions natively (games/gta5/spu, D-979 to D-983) against oracles built from the lifted code's
// helpers (gta5_audio_oracle.c): synthetic local stores and registers (CLAUDE.md rule 1), every class of value the
// SPU's float model treats apart, buffers at every word alignment. A native function either leaves what the oracle
// leaves (all of local store, the pc, the registers of its contract) or declines having changed nothing.

#include <catch_amalgamated.hpp>

#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/spu/context.h"
#include "pspocket/spu/fp.h"
#include "rage_audio.h"

extern "C" {
void ps_test_oracle_012b_062c8(SPUContext* ctx, uint8_t* ls);
void ps_test_oracle_012b_05b20(SPUContext* ctx, uint8_t* ls);
}

namespace {

using Fn = int (*)(SPUContext*, uint8_t*);
using Oracle = void (*)(SPUContext*, uint8_t*);

struct Rng {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<uint32_t>(s >> 16);
    }
};

// The kinds of floats a case draws from.
enum class Mix { audio, zeros, specials, silence };

uint32_t pick(Rng& r, Mix mix)
{
    static const uint32_t special[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x807FFFFFu, 0x00800000u, 0x80800000u,
        0x3F800000u, 0xBF800000u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFFFFFFFu,
        0x7F800001u, 0x3F7FFFFFu, 0x4B000000u, 0xCF000000u, 0x34000000u, 0x0A000000u};
    const auto audio = [&] { // |x| in [2^-40, 2^2), either sign
        return (r.next() & 0x807FFFFFu) | ((0x7Fu - 40u + r.next() % 42u) << 23);
    };
    switch (mix) {
    case Mix::audio: return audio();
    case Mix::zeros: return r.next() % 5 == 0 ? (r.next() & 1 ? 0x80000000u : 0u) : audio();
    case Mix::specials: return r.next() % 3 == 0 ? special[r.next() % (sizeof special / sizeof special[0])] : audio();
    case Mix::silence: return r.next() % 50 == 0 ? audio() : 0u;
    }
    return 0;
}

// A biquad case: coefficients in $4-$8 (four words each), the buffer's address in $3, the state's in $9, the count in
// $10; the job's word-insert controls at their addresses (relocated).
struct Case {
    std::vector<uint8_t> ls = std::vector<uint8_t>(PS_SPU_LS_SIZE);
    SPUContext ctx{};
};

void put_quad(uint8_t* ls, uint32_t at, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    ps_st32(ls + at, w0);
    ps_st32(ls + at + 4, w1);
    ps_st32(ls + at + 8, w2);
    ps_st32(ls + at + 12, w3);
}

void set_reg(SPUContext& ctx, int r, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    ctx.gpr[r].u32[3] = w0; // D-004: guest word i in host lane 3 - i
    ctx.gpr[r].u32[2] = w1;
    ctx.gpr[r].u32[1] = w2;
    ctx.gpr[r].u32[0] = w3;
}

Case biquad_case(Rng& r, Mix mix, uint32_t n, uint32_t align, uint32_t reloc)
{
    Case c;
    for (size_t i = 0; i < c.ls.size(); i += 4) ps_st32(c.ls.data() + i, r.next());
    put_quad(c.ls.data(), 0x15900 + reloc, 0x00010203u, 0x14151617u, 0x18191A1Bu, 0x1C1D1E1Fu);
    put_quad(c.ls.data(), 0x158F0 + reloc, 0x10111213u, 0x04050607u, 0x18191A1Bu, 0x1C1D1E1Fu);
    put_quad(c.ls.data(), 0x15AE0 + reloc, 0x10111213u, 0x14151617u, 0x08090A0Bu, 0x1C1D1E1Fu);
    put_quad(c.ls.data(), 0x15AF0 + reloc, 0x10111213u, 0x14151617u, 0x18191A1Bu, 0x0C0D0E0Fu);
    const uint32_t buf = 0x20000 + align, st = 0x30000;
    for (uint32_t i = 0; i < n; i++) ps_st32(c.ls.data() + buf + 4 * i, pick(r, mix));
    const Mix state_mix = mix == Mix::silence ? Mix::silence : mix == Mix::specials ? Mix::specials : Mix::audio;
    put_quad(c.ls.data(), st, pick(r, state_mix), pick(r, state_mix), pick(r, state_mix), pick(r, state_mix));
    for (int reg = 0; reg < 128; reg++) set_reg(c.ctx, reg, r.next(), r.next(), r.next(), r.next());
    // A stable-looking section in word 0 (a low-pass), whatever the mix in the other words.
    const Mix coef_mix = mix == Mix::specials ? Mix::specials : mix == Mix::zeros ? Mix::zeros : Mix::audio;
    const uint32_t b0 = 0x3CA89565u, b1 = mix == Mix::zeros ? 0u : 0x3D28956Au, b2 = 0x3CA89565u;
    const uint32_t a1 = 0xBFF761FFu, a2 = 0x3F7576A9u;
    const uint32_t coef[5] = {b0, b1, b2, a1, a2};
    for (int k = 0; k < 5; k++) {
        const uint32_t w0 = mix == Mix::specials && r.next() % 4 == 0 ? pick(r, coef_mix) : coef[k];
        set_reg(c.ctx, 4 + k, w0, pick(r, coef_mix), pick(r, coef_mix), pick(r, coef_mix));
    }
    set_reg(c.ctx, 3, buf, r.next(), r.next(), r.next());
    set_reg(c.ctx, 9, st, r.next(), r.next(), r.next());
    set_reg(c.ctx, 10, n, n, n, n);
    set_reg(c.ctx, 0, 0x1032C, 0, 0, 0);
    c.ctx.reloc = reloc;
    c.ctx.pc = 0x05B20 + reloc;
    return c;
}

struct Tally {
    int ran = 0, declined = 0;
};

// Runs the oracle and the native function on copies of a case: identical local store, pc and exact registers, or a
// decline that changed nothing.
void check(const Case& c, Fn native, Oracle oracle, const std::vector<int>& live, Tally& t)
{
    Case a = c, b = c;
    const uint64_t fp = ps_spu_fpenv_enter();
    oracle(&a.ctx, a.ls.data());
    const int ran = native(&b.ctx, b.ls.data());
    ps_spu_fpenv_leave(fp);
    if (!ran) {
        t.declined++;
        CHECK(std::memcmp(b.ls.data(), c.ls.data(), c.ls.size()) == 0);
        CHECK(std::memcmp(&b.ctx, &c.ctx, sizeof c.ctx) == 0);
        return;
    }
    t.ran++;
    CHECK(b.ctx.pc == a.ctx.pc);
    bool ls_same = std::memcmp(a.ls.data(), b.ls.data(), a.ls.size()) == 0;
    if (!ls_same) {
        for (uint32_t i = 0; i < PS_SPU_LS_SIZE; i += 4)
            if (ps_ld32(a.ls.data() + i) != ps_ld32(b.ls.data() + i)) {
                INFO("first difference at LS 0x" << std::hex << i << ": oracle " << ps_ld32(a.ls.data() + i) << ", native "
                                                 << ps_ld32(b.ls.data() + i));
                CHECK(ls_same);
                break;
            }
    }
    std::vector<int> exact = {0, 1};
    for (int r = 80; r < 128; r++) exact.push_back(r);
    exact.insert(exact.end(), live.begin(), live.end());
    for (const int r : exact) {
        INFO("register $" << r);
        CHECK(std::memcmp(&a.ctx.gpr[r], &b.ctx.gpr[r], 16) == 0);
    }
}

void biquad_suite(Fn native, Oracle oracle, uint32_t max_n, Tally& t)
{
    Rng r;
    const uint32_t counts[] = {0, 1, 2, 3, 4, 5, 7, 16, 17, 64, 255, 256};
    for (const Mix mix : {Mix::audio, Mix::zeros, Mix::specials, Mix::silence})
        for (const uint32_t n : counts)
            for (uint32_t align = 0; align < 16; align += 4)
                for (const uint32_t reloc : {0u, 0x400u}) {
                    if (n > max_n) continue;
                    check(biquad_case(r, mix, n, align, reloc), native, oracle, {6}, t);
                }
}

} // namespace

TEST_CASE("GTA V audio: the synthesizer's biquad (0x062C8) is the job's, bit for bit (D-979)", "[games]")
{
    Tally t;
    biquad_suite(ps_gta5_012b_062c8, ps_test_oracle_012b_062c8, 0x10000, t);
    CHECK(t.ran > 300);
    CHECK(t.declined == 0);
}

TEST_CASE("GTA V audio: the synthesizer's two biquad sections (0x05B20) are the job's, bit for bit (D-979)", "[games]")
{
    Tally t;
    biquad_suite(ps_gta5_012b_05b20, ps_test_oracle_012b_05b20, 1024, t);
    CHECK(t.ran > 300);
    // Declined: only a state with a denormal in it (the specials' and silence's).
    CHECK(t.declined < t.ran / 4);
}
