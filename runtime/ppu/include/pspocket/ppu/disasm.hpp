#pragma once

#include <string>

#include "pspocket/core/types.hpp"
#include "pspocket/ppu/decoder.hpp"

namespace pspocket::ppu {

// Disassembles one instruction at guest address pc with standard (GNU as / objdump style) mnemonics, including
// the common extended mnemonics: li, lis, mr, nop, not, slwi, srwi, clrlwi, rotlwi, sldi, srdi, clrldi, rotldi,
// cmpwi/cmpd..., mflr/mtctr..., blr/bctr/bctrl, beq/bne/blt/bge/bgt/ble/bso/bns (+lr/ctr), bdnz/bdz, crset/crclr,
// crmove/crnot, lwsync, trap. Branch targets print as absolute hex addresses. Unknown words print as ".long".
std::string disassemble(u32 raw, u32 pc);
std::string disassemble(const Inst& inst, u32 pc);

} // namespace pspocket::ppu
