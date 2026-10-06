#pragma once

// Lifted code of the firmware's modules (D-178). They sit at fixed addresses in the PRX region (PS_MEM_PRX_BASE, 256
// MB), far from the dispatch table's one range (the game's code), so registering them (ps_ppu_dispatch_register and
// _register_table, from a system library) puts them in a second table, sorted by address. The paths an address
// outside the first table takes (ps_ppu_dispatch_miss_stub, ps_ppu_jump_miss, ps_ppu_dispatch_find) look there before
// the miss policy. A module's lifted code runs only inside the ranges enabled after the loader checked that the
// module in guest memory is the one that was translated.
//
// Host only: translated code never includes this header (the game library ABI is unchanged).

#include <cstddef>

#include "pspocket/core/types.hpp"
#include "pspocket/ppu/runtime.h"

namespace pspocket::ppu {

bool in_prx_region(u32 addr);

// Lifted code in [lo, hi) may run (a verified module's code segment). Safe while guest threads run.
void prx_dispatch_enable(u32 lo, u32 hi);

// The lifted function at `addr`, if one is registered there and its range is enabled; else nullptr.
ps_ppu_fn prx_dispatch_find(u32 addr);

// Functions registered in the PRX region, and enabled ranges (for reports and tests).
size_t prx_dispatch_registered();
size_t prx_dispatch_enabled_ranges();

} // namespace pspocket::ppu
