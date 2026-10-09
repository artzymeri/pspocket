// setjmp and longjmp of sys_libc (D-572): guest code (interpreted) sets its registers, saves them with setjmp, changes
// them and calls down two frames to a function that longjmps back; setjmp's call answers again with the value given,
// the registers as they were, the frames between left behind, and the code goes on to its own return. No game data.

#include "hle_test_util.hpp"

#include "guest.hpp"

#include <bit>
#include <cstring>
#include <vector>

#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::ppu::as;

namespace {

constexpr u32 kCode = kData, kOuter = kData + 0x1000, kEnv = kData + 0x2000, kFloats = kData + 0x2400,
              kVrQ = kData + 0x2500, kRegs = kData + 0x2800, kOut = kData + 0x10000;
constexpr u32 kJbVr = 0x148; // where pspocket's jmp_buf keeps v20..v31 (sys_libc_process.cpp)
constexpr u32 kCr = 0x12345678;
constexpr double kF14 = 1.25, kF31 = -3.5, kClobbered = 99.0;

u32 known(u32 r) { return 0x00A10000u | r; } // r14..r31 at setjmp

// The guest program: outer(val, times, patch) sets r14..r31, f14, f31 and CR, calls setjmp(kEnv) and at each return
// appends {answer, r1, CR} to kOut (a count, then 16 bytes each from +16) and stores r14..r31, f14 and f31 at kRegs.
// While fewer than `times` longjmps came back, it changes those registers, copies kVrQ over the buffer's vector
// registers when `patch`, and calls middle(val), which calls inner(val), which longjmps(kEnv, val). Returns 0, or -1 if
// a longjmp returned.
void write_program(u8* b, u32 setjmp_opd, u32 longjmp_opd)
{
    std::vector<u32> c;
    const auto emit = [&](u32 w) { c.push_back(w); };
    const auto load = [&](u32 reg, u32 value) {
        emit(as::lis(reg, static_cast<s32>(value >> 16)));
        emit(as::ori(reg, reg, value & 0xFFFF));
    };
    const auto call_opd = [&](u32 opd) {
        load(12, opd);
        emit(as::lwz(0, 0, 12));
        emit(as::lwz(2, 4, 12));
        emit(as::mtctr(0));
        emit(as::bctrl());
    };
    const auto rel = [](usize from, usize to) {
        return static_cast<s32>(4 * (static_cast<s64>(to) - static_cast<s64>(from)));
    };

    // outer
    emit(as::mflr(0));
    emit(as::std_(0, 16, 1));
    emit(as::stdu(1, -0x100, 1));
    emit(as::std_(3, 0x80, 1));
    emit(as::std_(4, 0x88, 1));
    emit(as::std_(5, 0x90, 1));
    for (u32 r = 14; r < 32; r++) load(r, known(r));
    load(11, kFloats);
    emit(as::lfd(14, 0, 11));
    emit(as::lfd(31, 8, 11));
    load(12, kCr);
    emit(as::mtcrf(0xFF, 12));
    load(3, kEnv);
    call_opd(setjmp_opd);
    emit(as::mfcr(12)); // first, before anything sets CR
    load(11, kOut);
    emit(as::lwz(10, 0, 11));
    emit(as::slwi(9, 10, 4));
    emit(as::add(9, 11, 9));
    emit(as::std_(3, 16, 9));
    emit(as::stw(1, 24, 9));
    emit(as::stw(12, 28, 9));
    emit(as::addi(8, 10, 1));
    emit(as::stw(8, 0, 11));
    load(11, kRegs);
    for (u32 r = 14; r < 32; r++) emit(as::std_(r, static_cast<s32>(8 * (r - 14)), 11));
    emit(as::stfd(14, 0x90, 11));
    emit(as::stfd(31, 0x98, 11));
    emit(as::ld(4, 0x88, 1));
    emit(as::cmpd(0, 10, 4));
    const usize to_done = c.size();
    emit(0); // bge done
    for (u32 r = 14; r < 32; r++) emit(as::li(r, -1));
    load(11, kFloats);
    emit(as::lfd(14, 16, 11));
    emit(as::lfd(31, 16, 11));
    emit(as::li(12, 0));
    emit(as::mtcrf(0xFF, 12));
    emit(as::ld(5, 0x90, 1));
    emit(as::cmpdi(0, 5, 0));
    const usize to_call = c.size();
    emit(0); // beq call
    load(11, kVrQ - 8);
    load(12, kEnv + kJbVr - 8);
    emit(as::li(9, 24));
    emit(as::mtctr(9));
    const usize copy = c.size();
    emit(as::ld(10, 8, 11));
    emit(as::addi(11, 11, 8));
    emit(as::std_(10, 8, 12));
    emit(as::addi(12, 12, 8));
    emit(as::bc_rel(16, 0, rel(c.size(), copy))); // bdnz copy
    const usize call = c.size();
    emit(as::ld(3, 0x80, 1));
    const usize to_middle = c.size();
    emit(0); // bl middle
    emit(as::li(3, -1));
    const usize to_out = c.size();
    emit(0); // b out
    const usize done = c.size();
    emit(as::li(3, 0));
    const usize out = c.size();
    emit(as::addi(1, 1, 0x100));
    emit(as::ld(0, 16, 1));
    emit(as::mtlr(0));
    emit(as::blr());

    // middle: a frame the jump leaves behind
    const usize middle = c.size();
    emit(as::mflr(0));
    emit(as::std_(0, 16, 1));
    emit(as::stdu(1, -0x70, 1));
    emit(as::li(14, 0x55));
    emit(as::li(31, 0x55));
    const usize to_inner = c.size();
    emit(0); // bl inner
    emit(as::addi(1, 1, 0x70));
    emit(as::ld(0, 16, 1));
    emit(as::mtlr(0));
    emit(as::blr());

    // inner: longjmp(kEnv, val)
    const usize inner = c.size();
    emit(as::mflr(0));
    emit(as::std_(0, 16, 1));
    emit(as::stdu(1, -0x70, 1));
    emit(as::mr(4, 3));
    load(3, kEnv);
    call_opd(longjmp_opd);
    emit(as::trap());

    c[to_done] = as::bc_rel(4, 0, rel(to_done, done));
    c[to_call] = as::bc_rel(12, 2, rel(to_call, call));
    c[to_middle] = as::b_rel(rel(to_middle, middle), true);
    c[to_out] = as::b_rel(rel(to_out, out));
    c[to_inner] = as::b_rel(rel(to_inner, inner), true);
    REQUIRE(4 * c.size() <= kOuter - kCode);
    for (usize i = 0; i < c.size(); i++) ps_write32(b, kCode + 4 * static_cast<u32>(i), c[i]);
    ps_write32(b, kOuter, kCode);
    ps_write32(b, kOuter + 4, 0);
}

struct Answer {
    s64 value;
    u32 r1, cr;
};

struct Env {
    Fixture f;
    GuestThread g;
    u8* b;

    Env() : g(f, 0, "game"), b(f.base())
    {
        REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
        ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
        const u32 setjmp_opd = f.hle->link_function("sys_libc", recomp::nid_of("setjmp"), "setjmp");
        const u32 longjmp_opd = f.hle->link_function("sys_libc", recomp::nid_of("longjmp"), "longjmp");
        REQUIRE(setjmp_opd);
        REQUIRE(longjmp_opd);
        write_program(b, setjmp_opd, longjmp_opd);
        ps_write64(b, kFloats, std::bit_cast<u64>(kF14));
        ps_write64(b, kFloats + 8, std::bit_cast<u64>(kF31));
        ps_write64(b, kFloats + 16, std::bit_cast<u64>(kClobbered));
        for (u32 i = 0; i < 12 * 16; i++) ps_write8(b, kVrQ + i, static_cast<u8>(0x80 + i));
        // v20..v31 as the game had them: guest byte k of v(20 + n) is 16 * n + k (D-004: host byte 15 - k).
        for (u32 n = 0; n < 12; n++)
            for (u32 k = 0; k < 16; k++) g.ctx.vr[20 + n].u8[15 - k] = static_cast<u8>(16 * n + k);
    }

    u64 run(s32 val, u32 times, bool patch)
    {
        ps_write32(b, kOut, 0);
        return hle::call_guest(g.ctx, kOuter, {static_cast<u64>(static_cast<s64>(val)), times, patch ? 1u : 0u});
    }

    std::vector<Answer> answers()
    {
        std::vector<Answer> v;
        for (u32 i = 0; i < ps_read32(b, kOut); i++) {
            const u32 at = kOut + 16 + 16 * i;
            v.push_back({static_cast<s64>(ps_read64(b, at)), ps_read32(b, at + 8), ps_read32(b, at + 12)});
        }
        return v;
    }

    // r14..r31, f14 and f31 at the last return: as setjmp saw them.
    void check_registers()
    {
        for (u32 r = 14; r < 32; r++) {
            INFO("r" << r);
            CHECK(ps_read64(b, kRegs + 8 * (r - 14)) == known(r));
        }
        CHECK(std::bit_cast<double>(ps_read64(b, kRegs + 0x90)) == kF14);
        CHECK(std::bit_cast<double>(ps_read64(b, kRegs + 0x98)) == kF31);
    }
};

} // namespace

TEST_CASE("sys_libc: setjmp and longjmp, under sys_libc and sys_libm", "[hle][libc][setjmp]")
{
    for (const char* module : {"sys_libc", "sys_libm"}) {
        for (const char* name : {"setjmp", "longjmp"}) {
            INFO(module << "::" << name);
            CHECK(hle::registry().find(module, recomp::nid_of(name)) != nullptr);
        }
    }
}

TEST_CASE("sys_libc: longjmp comes back to setjmp's return two frames up, with its registers", "[hle][libc][setjmp]")
{
    Env e;
    CHECK(e.run(42, 1, false) == 0);
    const std::vector<Answer> a = e.answers();
    REQUIRE(a.size() == 2);
    CHECK(a[0].value == 0); // the call itself
    CHECK(a[1].value == 42);
    CHECK(a[1].r1 == a[0].r1); // outer's frame again; middle's and inner's left behind
    CHECK(a[0].cr == kCr);
    CHECK(a[1].cr == kCr);
    e.check_registers();
    // The vector registers in the buffer in guest byte order, as stvx stores them; restored as they were.
    for (u32 n = 0; n < 12; n++) {
        for (u32 k = 0; k < 16; k++) {
            INFO("v" << 20 + n << " byte " << k);
            CHECK(ps_read8(e.b, kEnv + kJbVr + 16 * n + k) == 16 * n + k);
            CHECK(e.g.ctx.vr[20 + n].u8[15 - k] == 16 * n + k);
        }
    }

    // longjmp(env, 0) makes setjmp answer 1; a negative value comes back as the int it is.
    CHECK(e.run(0, 1, false) == 0);
    REQUIRE(e.answers().size() == 2);
    CHECK(e.answers()[1].value == 1);
    CHECK(e.run(-7, 1, false) == 0);
    CHECK(e.answers()[1].value == -7);
}

TEST_CASE("sys_libc: longjmp restores the vector registers from the buffer", "[hle][libc][setjmp]")
{
    Env e;
    CHECK(e.run(5, 1, true) == 0); // the buffer's v20..v31 replaced by kVrQ's bytes before the longjmp
    REQUIRE(e.answers().size() == 2);
    for (u32 n = 0; n < 12; n++) {
        for (u32 k = 0; k < 16; k++) {
            INFO("v" << 20 + n << " byte " << k);
            CHECK(e.g.ctx.vr[20 + n].u8[15 - k] == static_cast<u8>(0x80 + 16 * n + k));
        }
    }
}

TEST_CASE("sys_libc: a thousand longjmps to one setjmp", "[hle][libc][setjmp]")
{
    Env e;
    CHECK(e.run(3, 1000, false) == 0);
    const std::vector<Answer> a = e.answers();
    REQUIRE(a.size() == 1001);
    u32 wrong = 0;
    for (usize i = 1; i < a.size(); i++) wrong += a[i].value != 3 || a[i].r1 != a[0].r1 || a[i].cr != kCr;
    CHECK(wrong == 0);
    e.check_registers();
}

TEST_CASE("sys_libc: setjmp on memory that is not the guest's saves nothing and answers 0", "[hle][libc][setjmp]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    CHECK(g.call("setjmp", 0xF0000000u) == 0);
}
