#pragma once

// Guest memory that nothing writes once the executable is loaded (D-1013): its segments read-only for the PPU and the
// SPUs (and the RSX). On a PS3 a store or a DMA there faults. Code identified there (an SPU job binary or module copied
// from it) is the same bytes every time it is loaded: the SPU program tracker need not hash it again to check.
// Set while the executable loads, before any thread runs; read from any thread.

#include "pspocket/core/types.hpp"

namespace pspocket {

// Marks [lo, hi) as never written from now on. At most 8 ranges; more are not marked (and keep being checked).
void add_immutable_range(u32 lo, u32 hi);
// Whether [addr, addr + size) lies in one marked range.
bool immutable_range(u32 addr, u32 size);
// Forgets every range (a new process; tests).
void clear_immutable_ranges();

} // namespace pspocket
