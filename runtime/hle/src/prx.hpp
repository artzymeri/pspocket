// sysPrxForUser's calls for other HLE modules: sys_prx.cpp's module calls (cellSysmodule's module files) and
// sysPrxForUser.cpp's exitspawns (sceNpDrmProcessExitSpawn), with the same checks, order and logs as the
// sysPrxForUser functions of the same names.

#pragma once

#include <memory>

#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/kernel.hpp"

namespace pspocket::hle {

// The module's ID (positive), or an error code.
s32 prx_load_module(PPUContext& ctx, u32 path, u64 flags);
// module_start (module_stop) runs on the calling thread; its result goes to the guest word at `result`.
s32 prx_start_module(PPUContext& ctx, u32 id, u32 args, u32 argp, u32 result);
s32 prx_stop_module(PPUContext& ctx, u32 id, u32 args, u32 argp, u32 result);
s32 prx_unload_module(PPUContext& ctx, u32 id);

// The process's module loader as lv2's PRX calls reach it (lv2::Kernel::set_module_host, D-527).
std::unique_ptr<lv2::ModuleHost> make_kernel_module_host(HleProcess& process);

// sys_game_process_exitspawn and sys_game_process_exitspawn2: the game ends and doesn't come back.
void game_process_exitspawn(PPUContext& ctx, u32 path, u32 argv, u32 envp, u32 data, u32 data_size, s32 prio,
    u64 flags);
void game_process_exitspawn2(PPUContext& ctx, u32 path, u32 argv, u32 envp, u32 data, u32 data_size, s32 prio,
    u64 flags);

} // namespace pspocket::hle
