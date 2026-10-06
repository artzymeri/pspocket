#pragma once

// cellGcmSys's calls for the other host modules that drive the RSX through it (cellResc, D-180): the same functions a
// game calls, with the same checks and logs (cellGcmSys.cpp).

#include "pspocket/core/types.hpp"
#include "pspocket/ppu/context.h"

namespace pspocket::hle::gcm {

s32 set_display_buffer(PPUContext& ctx, u8 id, u32 offset, u32 pitch, u32 width, u32 height);
// cellGcmSetFlip(context, id): CELL_OK, or CELL_GCM_ERROR_FAILURE (no room, a bad id).
s32 set_flip(PPUContext& ctx, u32 context, u32 id);
void set_wait_flip(PPUContext& ctx, u32 context);
void set_flip_handler(PPUContext& ctx, u32 handler);
void set_vblank_handler(PPUContext& ctx, u32 handler);
u32 get_flip_status(PPUContext& ctx);
void reset_flip_status(PPUContext& ctx);
u64 get_last_flip_time(PPUContext& ctx);

} // namespace pspocket::hle::gcm
