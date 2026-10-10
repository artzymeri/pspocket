#pragma once

// The cellFiber tests' fibers and contexts (D-568, D-597): a small guest routine that follows a script, so the log shows
// the order a scheduler ran them in. Shared by test_fiber.cpp and test_fiber_compare.cpp.

#include <catch_amalgamated.hpp>

#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/recomp/ppu/asm.hpp"

namespace pspocket::hle::test::fiber {

enum Op : u32 { END = 0, LOG = 1, CALL = 2, CALLQ = 3 };
struct Step {
    u32 op, a = 0, b = 0, c = 0;
};

// The script routine, its code at `code` and its descriptor at `routine`: r3 is the script, 16 bytes a step
// {op, a, b, c}. END returns a; LOG appends a to the log at `log` (a count, then the words); CALL calls the function
// whose descriptor is a with r3 = b, r4 = c (r5 to r7 zero) and appends what it answers (CALLQ: does not).
inline void write_routine(u8* base, u32 code, u32 routine, u32 log)
{
    namespace as = pspocket::recomp::ppu::as;
    std::vector<u32> c;
    const auto load = [&](u32 reg, u32 value) {
        c.push_back(as::lis(reg, static_cast<s32>(value >> 16)));
        c.push_back(as::ori(reg, reg, value & 0xFFFF));
    };
    c.push_back(as::mflr(0));
    c.push_back(as::std_(0, 16, 1));
    c.push_back(as::stdu(1, -0x80, 1));
    c.push_back(as::std_(31, 0x70, 1));
    c.push_back(as::std_(30, 0x68, 1));
    c.push_back(as::mr(31, 3));
    const usize loop = c.size();
    c.push_back(as::lwz(0, 0, 31));
    c.push_back(as::cmpwi(0, 0, END));
    const usize to_end = c.size();
    c.push_back(0); // beq end
    c.push_back(as::cmpwi(0, 0, LOG));
    const usize to_log = c.size();
    c.push_back(0); // beq log
    c.push_back(as::mr(30, 0)); // the op, to tell CALL from CALLQ after the call
    c.push_back(as::lwz(12, 4, 31));
    c.push_back(as::lwz(3, 8, 31));
    c.push_back(as::lwz(4, 12, 31));
    c.push_back(as::li(5, 0));
    c.push_back(as::li(6, 0));
    c.push_back(as::li(7, 0));
    c.push_back(as::lwz(0, 0, 12));
    c.push_back(as::lwz(2, 4, 12));
    c.push_back(as::mtctr(0));
    c.push_back(as::bctrl());
    c.push_back(as::cmpwi(0, 30, CALLQ));
    const usize to_next = c.size();
    c.push_back(0); // beq next
    c.push_back(as::mr(30, 3));
    const usize to_append = c.size();
    c.push_back(0); // b append
    const usize at_log = c.size();
    c.push_back(as::lwz(30, 4, 31));
    const usize append = c.size();
    load(11, log);
    c.push_back(as::lwz(12, 0, 11));
    c.push_back(as::addi(10, 12, 1));
    c.push_back(as::stw(10, 0, 11));
    c.push_back(as::slwi(12, 12, 2));
    c.push_back(as::add(12, 11, 12));
    c.push_back(as::stw(30, 4, 12));
    const usize next = c.size();
    c.push_back(as::addi(31, 31, 16));
    c.push_back(as::b_rel(static_cast<s32>(4 * (static_cast<s64>(loop) - static_cast<s64>(c.size())))));
    const usize end = c.size();
    c.push_back(as::lwz(3, 4, 31));
    c.push_back(as::ld(30, 0x68, 1));
    c.push_back(as::ld(31, 0x70, 1));
    c.push_back(as::addi(1, 1, 0x80));
    c.push_back(as::ld(0, 16, 1));
    c.push_back(as::mtlr(0));
    c.push_back(as::blr());
    const auto rel = [&](usize from, usize to) {
        return static_cast<s32>(4 * (static_cast<s64>(to) - static_cast<s64>(from)));
    };
    c[to_end] = as::bc_rel(12, 2, rel(to_end, end));
    c[to_log] = as::bc_rel(12, 2, rel(to_log, at_log));
    c[to_next] = as::bc_rel(12, 2, rel(to_next, next));
    c[to_append] = as::b_rel(rel(to_append, append));
    REQUIRE(code + 4 * c.size() <= routine);
    for (usize i = 0; i < c.size(); i++) ps_write32(base, code + 4 * static_cast<u32>(i), c[i]);
    ps_write32(base, routine, code);
    ps_write32(base, routine + 4, 0);
}

} // namespace pspocket::hle::test::fiber
