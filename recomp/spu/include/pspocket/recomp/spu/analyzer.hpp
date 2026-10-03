#pragma once

// SPU static analysis: function discovery (entries, brsl/brasl targets and code addresses formed by ila/il, to a
// fixpoint), per-function control flow (local branch targets, fall-through, resume points), and jump tables reached
// through bi (tables of code addresses next to an ila of their base). Every block start becomes a label: the run
// loop can resume lifted code there (return addresses, loop heads, instructions that may stop and restart).

#include <map>
#include <set>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/spu/image.hpp"
#include "pspocket/spu/decoder.hpp"

namespace pspocket::recomp::spu {

struct Function {
    u32 entry = 0;
    std::vector<u32> insns;      // every instruction address, ascending
    std::set<u32> labels;        // block starts (entry included)
    std::set<u32> calls;         // direct call targets
    std::set<u32> exits;         // instructions whose fall-through leaves the function (end of code)
    ::pspocket::spu::RegSet used;    // registers read or written
    ::pspocket::spu::RegSet written; // registers written
};

struct Diagnostic {
    u32 addr = 0;
    u32 word = 0;
    std::string message;
};

struct AnalyzerOptions {
    bool constant_entries = true; // treat ila/il immediates that point at code as function entries
    usize max_jump_table = 256;
};

struct Program {
    std::vector<Function> functions; // ascending entry
    std::set<u32> entries;
    std::vector<Diagnostic> unsupported; // reached but not supported: left to the interpreter at runtime
    std::vector<Diagnostic> warnings;

    const Function* find(u32 entry) const;
};

Program analyze(const LsImage& image, const AnalyzerOptions& options = {});

} // namespace pspocket::recomp::spu
