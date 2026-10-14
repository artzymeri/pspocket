#pragma once

// SPU static analysis: function discovery (entries, brsl/brasl targets and code addresses formed by ila/il, to a
// fixpoint), per-function control flow (local branch targets, fall-through, resume points), and jump tables reached
// through bi (tables of code addresses next to an ila of their base). Every block start becomes a label: the run
// loop can resume lifted code there (return addresses, loop heads, instructions that may stop and restart). Seeds
// (LsImage::seeds) inside a function's code become labels of it; the others start functions.

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
    // Labels an indirect jump of the function itself may go to: jump table targets, code addresses formed by
    // constants, seeds (D-219). Any other target of an indirect jump leaves to the run loop, which enters the right
    // function there.
    std::set<u32> dynamic;
    std::set<u32> calls;         // direct call targets
    std::set<u32> exits;         // instructions whose fall-through leaves the function (end of code)
    ::pspocket::spu::RegSet used;    // registers read or written
    ::pspocket::spu::RegSet written; // registers written
    // Found only by AnalyzerOptions::discover: possibly data decoded as code, so a DMA over it does not make the
    // program stale (the lifter leaves it out of the program's code words, D-155).
    bool discovered = false;
    // Reached only from an ila/il constant pointing into the code (a data address as often as a function pointer: the
    // shuffle patterns of Assassin's Creed's jobs, run 115), not from an entry, a seed or a direct call of code that
    // is: left out of the code words too (D-204).
    bool speculative = false;
};

struct Diagnostic {
    u32 addr = 0;
    u32 word = 0;
    std::string message;
};

struct AnalyzerOptions {
    bool constant_entries = true; // treat ila/il immediates that point at code as function entries
    usize max_jump_table = 256;
    // Position independent code (D-081): absolute branch targets (bra, brasl) are not the image's code wherever it
    // sits, so they are not followed.
    bool relocatable = false;
    // Code reached only through registers (jump tables not recognised, function pointers: the common case in
    // position independent jobs), found without a run's seeds (D-155): every instruction after one that never falls
    // through (an unconditional branch, a return, a stop), padding skipped, that nothing reached yet is traced as a
    // seed is. Data that decodes as code there is lifted and never run.
    bool discover = false;
};

struct Program {
    std::vector<Function> functions; // ascending entry
    std::set<u32> entries;
    std::vector<Diagnostic> unsupported; // reached but not supported: left to the interpreter at runtime
    std::vector<Diagnostic> warnings;

    const Function* find(u32 entry) const;
};

Program analyze(const LsImage& image, const AnalyzerOptions& options = {});

// A channel write the lifter makes a plain store of ctx (D-959): SPU_WrSRR0 and the MFC's command parameters (MFC_LSA,
// MFC_EAH, MFC_EAL, MFC_Size, MFC_TagID), which ps_spu_wrch only stores. No runtime call, no way out.
bool inline_channel_write(const ::pspocket::spu::Inst& inst);

// brsl $rt, .+4: the code takes its own address (position independent code): rt is written and the code falls
// through, no call (D-963).
bool get_pc(const ::pspocket::spu::Inst& inst, u32 pc);

// Whether the code for a supported instruction that calls the runtime (RegUse::runtime) may leave for the run loop and
// be resumed at the instruction itself, which then starts a block (a label): a channel that may block (the run loop
// takes a stop request while it waits and executes it again), a halt, an interrupt taken at an indirect branch, a stop.
// The inline channel writes never leave, and MFC_WrTagMask, MFC_WrTagUpdate and rchcnt never block: they leave only
// on an SPU exception, which ends the thread (D-959).
bool restarts_in_place(const ::pspocket::spu::Inst& inst);

} // namespace pspocket::recomp::spu
