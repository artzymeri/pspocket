#pragma once

// fs_utility_init (fs_utility_init.cpp): what its comparison with Sony's module needs.

#include "pspocket/core/types.hpp"
#include "pspocket/ppu/context.h"

namespace pspocket::hle {

// The disk object 6B5896B0 asks lv2 about (its device's information at +0x18), made if it is not yet: its address.
u32 fs_utility_init_disk(PPUContext& ctx);
inline constexpr u32 kFsUtilityInitDiskSize = 0x60;

} // namespace pspocket::hle
