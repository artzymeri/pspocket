#pragma once

// SPU lifter: analysed functions -> C11 translation units.
//
// One C function per SPU function, void spu_<hash>_<start>_<entry>(SPUContext* ctx, uint8_t* ls), entered at any of its
// block starts (a switch on ctx->pc). Registers it uses live in ps_spu_vec locals; local branches are gotos (loop
// back edges poll ctx->attention); direct calls into lifted functions are C calls that continue only when the
// callee returned to the expected address (anything else returns to the run loop); indirect branches switch over
// the labels such branches may reach (Function::dynamic) and otherwise return to the run loop, which dispatches
// through the program table or falls back to the interpreter (logging the address). Called at its entry, a function
// loads only the registers it may read before writing them; every way out stores only those it may have written
// (D-219).
//
// Output of lift() for options.name = N:
//   N.h          prototypes, N_program, N_register()
//   N_0.c ...    the lifted functions (options.tu_count units)
//   N_reg.c      the program descriptor (key, code range, entries) and N_register()

#include <string>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/spu/analyzer.hpp"
#include "pspocket/recomp/spu/image.hpp"

namespace pspocket::recomp::spu {

struct LiftOptions {
    std::string name = "spu_lifted";
    u32 tu_count = 1;
    bool comments = true;
    u8 xfloat = 0; // PS_SPU_XFLOAT_APPROXIMATE or PS_SPU_XFLOAT_ACCURATE (pspocket/spu/fp.h)
    // Position independent (D-081): runs ctx->reloc bytes past the image's base. Every address the code derives from
    // its own (pcs, links, relative branch and load targets) adds it; absolute branches leave to the run loop.
    bool relocatable = false;
};

struct OutputFile {
    std::string name;
    std::string content;
};

struct LiftStats {
    usize functions = 0;
    usize instructions = 0;
    usize labels = 0;
    usize unsupported = 0; // left to the interpreter
};

struct LiftResult {
    std::vector<OutputFile> files;
    LiftStats stats;
    u64 hash = 0;
    std::vector<u32> code_words; // the program's code words (a bit per word, ps_spu_program)
    u64 code_hash = 0;           // ps_spu_code_hash of the image over them (D-208)
};

LiftResult lift(const LsImage& image, const Program& program, const LiftOptions& options);

// "spu_<hash>_<code start>_<entry>": the program key (16 hex digits), its LS code start and the entry (5 hex digits
// each), with an "r" before the code start for a relocatable program. The code start keeps apart the programs one
// binary makes at different LS addresses (D-076), the "r" a relocatable lift from a fixed one of the same image.
std::string function_symbol(u64 hash, u32 code_start, u32 entry, bool relocatable = false);

// Step mode, for the differential fuzz test: one instruction as a function with the same code generation as
// lift(), except that control transfers store their target in ctx->pc and return (calls don't call), and every
// other instruction stores pc + 4. The instruction must be supported. Relocatable: lifted at pc, it behaves as the
// instruction at pc + ctx->reloc.
std::string lift_step_function(std::string_view symbol, u32 insn, u32 pc, u8 xfloat, bool relocatable = false);

// Writes files into dir (created if needed); rewrites a file only when its content changed.
bool write_files(const std::string& dir, const std::vector<OutputFile>& files, std::string& error);

} // namespace pspocket::recomp::spu
