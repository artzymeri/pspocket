#pragma once

// Diagnostics of the SPU runtime for stall reports (D-1091). Not part of the lifted code's ABI (runtime.h is).

#include "pspocket/spu/context.h"

extern "C" {
// The last DMA command of a context with a host thread: its command, EA, size and SPU pc, and how many it has made
// (0 without a host). Read racily: for reports.
uint64_t ps_spu_last_dma(SPUContext* ctx, uint32_t* cmd, uint32_t* ea, uint32_t* size, uint32_t* pc);
}
