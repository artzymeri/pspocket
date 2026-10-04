#pragma once

// PPU lifter: analysed functions -> C11 translation units (D-009 signature, registers in locals, direct calls,
// goto for local branches, a switch for recognised jump tables, runtime calls for everything else).
//
// Output of lift() for options.name = N:
//   N.h           prototypes, N_register(), N_unregister(), N_function_count
//   N_0.c ...     the lifted functions, options.tu_count translation units
//   N_reg.c       the dispatch-table registration unit

#include <string>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/ppu/analyzer.hpp"
#include "pspocket/recomp/ppu/code_image.hpp"

namespace pspocket::recomp::ppu {

struct LiftOptions {
    std::string name = "ppu_lifted"; // file stem and prefix of the registration functions
    std::string prefix;              // prefix of the lifted function symbols: <prefix>f_XXXXXXXX
    u32 tu_count = 1;
    bool comments = true;            // "address: word  disassembly" before every instruction
};

struct OutputFile {
    std::string name;
    std::string content;
};

struct LiftStats {
    usize functions = 0;
    usize instructions = 0;
    usize fallbacks = 0;   // unsupported instructions lifted as interpreter calls
    usize jump_tables = 0;
    usize indirect_jumps = 0; // bctr without a recognised table
    usize illegal = 0;
    usize entry_not_first = 0; // functions whose entry is above code they branch back into
};

struct LiftResult {
    std::vector<OutputFile> files;
    LiftStats stats;
};

LiftResult lift(const CodeImage& image, const Program& program, const LiftOptions& options);

// "<prefix>f_XXXXXXXX" (upper-case hex, D-009).
std::string function_symbol(std::string_view prefix, u32 addr);

// Step mode, for the differential fuzz test: one instruction as a function with the same code generation as
// lift(), except that branches store their target in ctx->cia instead of transferring control (non-branches
// store pc + 4). The instruction must be supported.
std::string lift_step_function(std::string_view symbol, u32 insn, u32 pc);

// Writes files into dir (created if needed); rewrites a file only when its content changed.
bool write_files(const std::string& dir, const std::vector<OutputFile>& files, std::string& error);

} // namespace pspocket::recomp::ppu
