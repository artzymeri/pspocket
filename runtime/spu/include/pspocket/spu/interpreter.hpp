#pragma once

// SPU interpreter: the test oracle for the lifter and the runtime fallback for code that was not lifted.
// Semantics follow the SPU ISA v1.2 and RPCS3 (rpcs3/Emu/Cell/SPUInterpreter.cpp, and SPULLVMRecompiler.cpp for the
// default floating-point model); the floating-point model is documented in pspocket/spu/fp.h. The implementation is
// deliberately scalar and written in guest element order, independent of the vectorised helpers lifted code uses
// (pspocket/spu/lifted.h), so the differential fuzz test compares two implementations.

#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/spu/context.h"
#include "pspocket/spu/decoder.hpp"

namespace pspocket::spu {

// Executes the instruction at ctx.pc and advances ctx.pc (taken branches set it to their target). Returns false
// when execution must stop; ctx.stop_reason says why (stop-and-signal, halt, exception, blocked channel, stop
// request). Requires the SPU FP environment (pspocket/spu/fp.h) for floating-point instructions.
bool step(SPUContext& ctx);
// Executes an already decoded instruction; ctx.pc must be its address.
bool execute(SPUContext& ctx, const Inst& inst);

struct RunOptions {
    u64 max_steps = 0;          // 0 = no limit (PS_SPU_STOP_LIMIT when reached)
    bool stop_at_lifted = false; // return before executing an address the attached program has lifted code for
                                 // (checked after taken branches)
};

struct RunResult {
    u64 steps = 0;
    bool reached_lifted = false; // stopped because ctx.pc has lifted code (stop_reason stays NONE)
};

// Interprets until execution stops (ctx.stop_reason != NONE), the step limit, or (stop_at_lifted) lifted code.
// Also returns when ctx.attention is set (checked on taken branches).
RunResult run(SPUContext& ctx, const RunOptions& options);

// Every op the interpreter executes. The differential fuzz test fails when one of them has no fuzz cases.
std::vector<Op> supported_ops();

} // namespace pspocket::spu
