#pragma once

// liblv2coredump (sys_lv2coredump, D-943): its start, as libsysmodule runs it when the executable declares
// SYS_PROCESS_CRASH_DUMP_ENABLED. pspocket's process start does not call it yet (D-943: which module a process gets is
// the loader's decision still to make); tests call it as the LLE loader starts Sony's module.

#include "pspocket/core/types.hpp"
#include "pspocket/ppu/context.h"

namespace pspocket::hle {

// module_start(args, argp): the configuration from argp's 64 bytes (args 0x40), or from the process's
// _sys_process_get_paramsfo when there are none; 0, or the set-up's error.
s32 lv2coredump_start(PPUContext& ctx, u64 args, u64 argp);

} // namespace pspocket::hle
