#pragma once

// Synthetic PPU programs built in code with the assembler (CLAUDE.md rule 1: test inputs are synthetic). Used by
// the program-level tests (tests/ppu), the self-test benchmark generator and anything else that needs real
// multi-function guest code without game data.

#include <map>
#include <span>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/ppu/code_image.hpp"

namespace pspocket::recomp::ppu::synthetic {

struct Program {
    u32 base = 0;
    std::vector<u8> bytes;            // image loaded at base
    std::vector<Seed> seeds;          // functions to lift (with TOC)
    std::map<std::string, u32> symbols; // functions and data labels
    u32 toc = 0;                      // r2 the code expects (0 when unused)

    u32 symbol(const std::string& name) const { return symbols.at(name); }
    CodeImage image() const { return CodeImage(base, bytes); }
};

// The program-level test suite. Functions (C-like signatures; PPC64 ELF ABI: args r3.., result r3 or f1):
//   sum_to(n)                 loop with mtctr/bdnz
//   fib(n)                    recursive, stack frames, mflr/mtlr, std/ld
//   memcpy_bytes(d, s, n)     lbzu/stbu loop, beqlr
//   memset_bytes(d, c, n)     stbx loop
//   copy_dwords(d, s, n)      ldx/stdx loop
//   bubble_sort(a, n)         int32, nested loops, cmpw/ble
//   sw_rel(x)                 jump table: cmplwi/bgt, lis/addi @ha/@l, slwi, lwax, add, mtctr, bctr (relative)
//   sw_toc(x)                 jump table: table address loaded from the TOC (ld rX, off(r2)), rldic scaling
//   sw_snc(x)                 jump table, SNC style: clrlwi/cmplwi on a byte index, lwz of the table from the TOC,
//                             rlwinm mask-and-scale, lwzx + extsw + add (relative entries)
//   entry_above(x)            sets r7 and branches back into a body below its entry (x + 5)
//   vm(code)                  bytecode interpreter: table base set once in the prologue, byte opcodes, four
//                             dispatch sites (0 return, 1 +1, 2 *2, 3 +10, 4 add_one across a call); it branches to
//                             vm_exit (not seeded) to restore its registers and return
//   sw_abs(x)                 jump table: lis/ori, lwzx, absolute entries
//   add_one(x), times_three(x)      function-pointer targets (seeded, never bl'd)
//   secret_fn(x)              function-pointer target deliberately NOT seeded (dispatch miss -> interpreter)
//   apply(fn, x)              bctrl through a register
//   apply_table(i, x)         bctrl through a table of function addresses
//   dot_sqrt(a, b, n)         FP: lfdx, fmadd, fsqrt (result f1)
//   sum_floats_to_int(a, n)   FP: lfsx, fadds, fctiwz, stfiwx
//   fmax(f1, f2)              fcmpu + branch
//   add256(d, a, b)           addc/adde/addze carry chain (4 x 64-bit limbs, least significant first)
//   sub256(d, a, b)           subfc/subfe borrow chain; returns 0 or -1
//   atomic_add(p, v)          lwarx/stwcx. retry loop
//   syscall_test(x)           li r11, 77; sc
//   trap_test(x)              twi eq (traps when x == 13)
//   pc_rel()                  bcl 20,31,$+4 / mflr PC-relative load
//   str_copy13(d, s)          lswi/stswi
//   cr_games(x, y)            cmp*, cror, crxor, mcrf, creqv, mfcr, mtcrf
//   gcd(a, b)                 tail recursion as a branch to its own entry
//   tail_calls(x)             b / conditional b into other functions
//   driver(n)                 nested calls into sum_to, fib, sw_rel
//   cond_ctr(x, fn)           bnectr (conditional indirect tail jump)
//   blrl_call(fn, x)          call through LR
//   cond_call(x)              beql (conditional call)
//   count_bdzlr(n)            bdzlr (conditional return on CTR)
//   trap_resume(x)            unconditional trap; lifted code resumes in the interpreter
//   fall_a(x) / fall_b(x)     fall_a runs into fall_b (a fall-through tail call)
Program build_test_programs(u32 base);

// Self-test benchmark: bench(buf, words, iterations, fib_n) runs `iterations` times
//   acc += checksum(buf, words); acc ^= fib(fib_n); acc = rotl64(acc, 7)
// where checksum() walks the buffer (lwzx/rotlwi/xor/add/stwx, mtctr/bdnz) and rewrites it.
Program build_selftest_bench(u32 base);

// Native reference of bench(); buffer holds the guest words (host order) and is updated like the guest's.
u64 bench_reference(std::span<u32> buffer, u32 iterations, u32 fib_n);
u64 fib_reference(u32 n);

} // namespace pspocket::recomp::ppu::synthetic
