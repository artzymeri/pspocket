#pragma once

// SPU lifter: analysed functions -> C11 translation units.
//
// One C function per SPU function, void spu_<hash8>_<entry>(SPUContext* ctx, uint8_t* ls), entered at any of its
// block starts (a switch on ctx->pc). Registers it uses live in ps_spu_vec locals; local branches are gotos (loop
// back edges poll ctx->attention); direct calls into lifted functions are C calls that continue only when the
// callee returned to the expected address (anything else returns to the run loop); indirect branches switch over
// the function's labels and otherwise return to the run loop, which dispatches through the program table or falls
// back to the interpreter (logging the address).
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
};

LiftResult lift(const LsImage& image, const Program& program, const LiftOptions& options);

// "spu_<hash8>_<entry>": hash8 = top 32 bits of the program key, entry = 5 hex digits.
std::string function_symbol(u64 hash, u32 entry);

// Step mode, for the differential fuzz test: one instruction as a function with the same code generation as
// lift(), except that control transfers store their target in ctx->pc and return (calls don't call), and every
// other instruction stores pc + 4. The instruction must be supported.
std::string lift_step_function(std::string_view symbol, u32 insn, u32 pc, u8 xfloat);

// Writes files into dir (created if needed); rewrites a file only when its content changed.
bool write_files(const std::string& dir, const std::vector<OutputFile>& files, std::string& error);

} // namespace pspocket::recomp::spu
