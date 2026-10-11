#pragma once

// The SPU printf's text (sys_libc_format.cpp, D-905), for spu_printf's server (sys_libc_process.cpp).

#include "libc_printf.hpp"
#include "pspocket/ppu/context.h"

namespace pspocket::hle::libc {

// What spu_thread_printf prints for the SPU thread's arguments at ls_args in its local store: the text and the count.
PrintResult<char> spu_thread_print_text(PPUContext& ctx, u32 spu, u32 ls_args);
// spu_thread_printf itself: the text to the TTY's SPU channel; the count, -1 when it cannot be written.
s32 spu_thread_print_tty(PPUContext& ctx, u32 spu, u32 ls_args);

} // namespace pspocket::hle::libc
