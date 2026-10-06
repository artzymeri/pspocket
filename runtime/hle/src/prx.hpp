// sys_prx.cpp's module calls for other HLE modules (cellSysmodule's module files): the same checks, order and logs as
// the sysPrxForUser functions of the same names.

#pragma once

#include "pspocket/hle/hle.hpp"

namespace pspocket::hle {

// The module's ID (positive), or an error code.
s32 prx_load_module(PPUContext& ctx, u32 path, u64 flags);
// module_start (module_stop) runs on the calling thread; its result goes to the guest word at `result`.
s32 prx_start_module(PPUContext& ctx, u32 id, u32 args, u32 argp, u32 result);
s32 prx_stop_module(PPUContext& ctx, u32 id, u32 args, u32 argp, u32 result);
s32 prx_unload_module(PPUContext& ctx, u32 id);

} // namespace pspocket::hle
