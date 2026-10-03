#pragma once

// SPU disassembler (spu-as syntax: $lr, $sp, $N registers; branch and LS targets as absolute LS addresses).

#include <string>
#include <string_view>

#include "pspocket/core/types.hpp"

namespace pspocket::spu {

std::string disassemble(u32 insn, u32 pc);
std::string_view channel_name(u32 channel); // "SPU_RdInMbox", or "" for an unnamed channel
std::string reg_name(u32 r);                // "$lr", "$sp", "$3"

} // namespace pspocket::spu
