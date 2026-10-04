#pragma once

// PPU static analysis: function discovery (seeds + bl targets, to a fixpoint), per-function control flow
// (local branch targets, fall-through, conditional branches), tail calls, function extent, and bctr jump-table
// recognition for the PPC64 GCC and SNC switch patterns. Anything not recognised stays an indirect jump through the
// dispatch table at runtime.

#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/ppu/code_image.hpp"

namespace pspocket::recomp::ppu {

struct JumpTable {
    u32 bctr = 0;          // address of the bctr
    u32 table = 0;         // address of the first entry
    bool relative = false; // entries are signed offsets from the table address (lwax, or lwzx + extsw; then add)
    bool bounded = false;  // entry count from the cmplwi/bgt bounds check (else: scanned until an invalid entry)
    std::vector<u32> targets; // one per entry, in table order
};

// How a basic block ends.
enum class BlockEnd : u8 {
    FallThrough, // into the next block (which starts at a branch target)
    Jump,        // unconditional local branch
    Conditional, // local conditional branch: taken + fall-through successors
    CondExit,    // conditional return / tail call / indirect jump: fall-through successor only
    Return,      // blr
    TailCall,    // b / fall-through into another function
    Switch,      // bctr through a recognised jump table
    Indirect,    // bctr without a table, or a branch out of the image
    Stop,        // unconditional trap or illegal instruction
};

struct Block {
    u32 start = 0;
    u32 end = 0;            // one past the last instruction
    BlockEnd kind = BlockEnd::FallThrough;
    std::vector<u32> succs; // local successors (block starts)
};

struct Function {
    u32 entry = 0;
    std::optional<u32> toc;
    std::vector<u32> insns;       // every instruction address, ascending
    std::set<u32> labels;         // targets of local branches and jump tables
    std::set<u32> fallthrough_calls; // A: the instruction at A - 4 falls through into function entry A (a tail call)
    std::map<u32, JumpTable> jump_tables; // keyed by the bctr address
    std::set<u32> calls;          // direct call targets
    std::set<u32> tail_calls;     // b / bc into other functions
    std::vector<Block> blocks;    // the control-flow graph, ascending start
    u32 end = 0;                  // one past the last instruction
    // The lifter keeps the entry LR/SP for interpreter fallbacks: indirect jumps, unconditional traps, leaving the
    // image by fall-through.
    bool needs_entry_state = false;
};

struct AnalyzerOptions {
    std::optional<u32> toc;           // r2 for functions whose seed has no TOC (raw images)
    usize max_function_insns = 1u << 20;
};

struct Diagnostic {
    u32 addr = 0;
    u32 word = 0;
    std::string message;
};

struct Program {
    std::vector<Function> functions; // ascending entry
    std::set<u32> entries;
    std::vector<Diagnostic> unsupported; // decoded but not supported: lifted as interpreter fallbacks
    std::vector<Diagnostic> illegal;     // undecodable words reached by control flow
    std::vector<Diagnostic> warnings;

    const Function* find(u32 entry) const;
};

Program analyze(const CodeImage& image, std::span<const Seed> seeds, const AnalyzerOptions& options = {});

} // namespace pspocket::recomp::ppu
