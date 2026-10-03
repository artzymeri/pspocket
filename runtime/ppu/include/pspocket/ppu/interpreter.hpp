#pragma once

// PPU interpreter: the test oracle for the lifter and the runtime fallback for code that was not lifted.
// Semantics follow RPCS3's interpreter (rpcs3/Emu/Cell/PPUInterpreter.cpp); deviations are documented where they
// occur (FPSCR model: pspocket/ppu/fpu.h; VMX model: pspocket/ppu/vmx.h; reservations: pspocket/ppu/runtime.h).

#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/ppu/context.h"
#include "pspocket/ppu/decoder.hpp"

namespace pspocket::ppu {

// Executes the instruction at ctx.cia and advances cia (taken branches set it to their target). sc and traps call
// the runtime hooks (pspocket/ppu/runtime.h) and continue at cia + 4. Unsupported or illegal instructions log the
// address and the disassembly, then abort: there is nothing to fall back to.
void step(PPUContext& ctx, u8* base);
// Executes an already decoded instruction; ctx.cia must be its address.
void execute(PPUContext& ctx, u8* base, const Inst& inst);
// The VMX part of execute() (interpreter_vmx.cpp): registers, VSCR and memory, never cia.
void execute_vmx(PPUContext& ctx, u8* base, const Inst& inst);

struct RunOptions {
    u64 max_steps = ~0ull;       // stop after this many instructions
    bool stop_at_return = false; // stop before executing at stop_lr when r1 == stop_sp
    u64 stop_lr = 0;
    u64 stop_sp = 0;
    bool call_lifted = false;    // interop: a branch to a registered lifted function calls it, then continues at LR
};

enum class StopReason : u8 { Returned, StepLimit };

struct RunResult {
    u64 steps = 0;        // interpreted instructions
    u64 lifted_calls = 0; // lifted functions entered (call_lifted)
    StopReason reason = StopReason::StepLimit;
};

RunResult run(PPUContext& ctx, u8* base, const RunOptions& options);

// Every instruction form (op + Rc/OE/LK/AA variant) the interpreter executes. The differential fuzz test fails
// when one of them has no fuzz cases.
struct Form {
    Op op;
    u32 variant;
};
std::vector<Form> supported_forms();

} // namespace pspocket::ppu
