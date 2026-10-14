#pragma once

// Interprocedural register liveness over a whole SPU program (D-965): the registers that may be read before they are
// written on some path from an address on, across calls (each callee summarised by what it may read before writing
// and what it writes on every path to its returns) and across returns (to every return site of the frames the
// address belongs to). Unlike the lifter's per-function analysis (which counts every register a way out stores as
// live), it follows the program to the end of its run: a register nothing reads again is dead.
//
// What it answers: when a native replacement takes over a function, which registers its callers (and their callers)
// may still read when it returns, so the replacement must leave them exactly as the function does.
//
// Frames: an entry called with the link register ($lr) — a direct `brsl $lr` target, an indirect call's target (given:
// the analysis cannot know them), or the program's entry (returns leave the program: nothing live). An instruction
// belongs to the frames whose entries reach it without a call ($lr link); the get-pc idiom (brsl $x, next) is not a
// call. `bi $lr` returns to every return site of its frames. Conservative where it cannot know: an indirect jump with
// no known target, a branch out of the program's code or a call with no known target make every register live there,
// and are reported.

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/spu/analyzer.hpp"
#include "pspocket/recomp/spu/image.hpp"
#include "pspocket/spu/decoder.hpp"

namespace pspocket::recomp::spu {

struct LivenessOptions {
    // Targets of indirect calls (bisl) by the call's address. A call with none listed may go to any frame entry the
    // analyzer found (every function's entry).
    std::map<u32, std::vector<u32>> indirect_calls;
    // Targets of indirect jumps (bi/biz... through a register other than $lr) by the jump's address, beside the
    // labels the analyzer found such jumps may reach (Function::dynamic).
    std::map<u32, std::vector<u32>> indirect_jumps;
    // Indirect jumps that go to another function's entry (a call through a pointer in tail position), by address, with
    // their targets (none listed: any function). The callee returns where the jumping frame would.
    std::map<u32, std::vector<u32>> tail_calls;
    // An indirect call (or tail call) with no targets given: an ABI callee (reads $0, $1, $80-$127 and the argument
    // registers the caller wrote just before; writes nothing it must) instead of any function of the program — the
    // fragments jump tables reach are no callbacks. For calls a run never made (their targets unknown).
    bool abi_unknown_calls = false;
    // Variadic functions (a printf to the PPU that saves every argument register): a direct call of one reads only the
    // argument registers the caller wrote just before it (and $0, $1, $80-$127).
    std::set<u32> abi_callees;
};

struct Liveness {
    // Registers live before each instruction of the program (the context liveness: returns go to the callers).
    std::map<u32, ::pspocket::spu::RegSet> live_in;
    // Per frame entry: what it may read before writing (to its returns) and what it writes on every path to them.
    std::map<u32, ::pspocket::spu::RegSet> uses, kills;
    // Return sites of each frame entry (0xFFFFFFFF: returns out of the program).
    std::map<u32, std::set<u32>> returns;
    // Where the analysis had to assume every register live, and why.
    std::vector<std::string> unknowns;

    ::pspocket::spu::RegSet at(u32 addr) const;
    // A path from `from` to an instruction (or callee) that reads `reg` before anything writes it, as text.
    std::function<std::string(u32 from, u32 reg)> why;
};

Liveness analyze_liveness(const LsImage& image, const Program& program, const LivenessOptions& options = {});

} // namespace pspocket::recomp::spu
