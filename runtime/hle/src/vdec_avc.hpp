#pragma once

// libavcdec, the H.264 codec core, pspocket's own (D-933; libmvcdec, the MVC core, is the same design for one view or
// two): its memory's layout and its frame buffers (vdec_avc.cpp). Its PPU side keeps a context at the end of the
// decoder's memory, command queues and a helper thread that gives the caller's callback the SPU side's answers.
// Nothing here is Sony's code or data (CLAUDE.md rule 1).

#include <array>
#include <optional>

#include "pspocket/core/types.hpp"

namespace pspocket::hle {
class Registry;
}

namespace pspocket::hle::vdec::avc {

// The decoder's memory for a level and the caller's limits (avcDecQueryMemory): the fourteen blocks of its SPU side's
// working memory (the frame buffers the fifth), their sum with the level's extra bytes (the dynamic part, after the
// fixed 0x298180 bytes of task contexts, queues, SPURS and taskset), the whole size, and the frame buffers' offset in
// the dynamic part.
struct Layout {
    std::array<u32, 14> blocks{};
    u32 dynamic = 0, size = 0, frames = 0, au_buffer = 0;
    u32 frame_mbs = 0; // the largest frame it holds, in macroblocks
};

// The level (10 to 42), the frame buffer count (0: as many as the level's buffer holds, at most 16) and the largest
// frame in macroblocks (0, 0: the level's worst case); none for a combination libavcdec refuses.
std::optional<Layout> layout(u32 level, u32 buffers, u32 width_mbs, u32 height_mbs);

// A frame buffer slot's bytes and Sony's layout of a picture in it (D-196).
u32 frame_slot_bytes(u32 width, u32 height);
void write_frame(u8* base, u32 at, u32 width, u32 height, const u8* y, const u8* u, const u8* v);

void register_avcdec(Registry& r);

} // namespace pspocket::hle::vdec::avc
