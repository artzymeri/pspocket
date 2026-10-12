// cellSailRec's video pipeline (D-918, part 8): the pictures of a game's video feeder converted, scaled and encoded on
// the game's SPURS, as libsail_rec (fw 4.92) runs them. The later parts of D-918 make it.

#include "cellSailRec.hpp"
#include "pspocket/hle/hle.hpp"

namespace pspocket::hle::sailrec {

void video_pipeline_start(PPUContext& ctx, u32, u32, u32, u64) { later(ctx, "a video pipeline's start"); }
void video_pipeline_stop(PPUContext& ctx, u32, u32, u64) { later(ctx, "a video pipeline's stop"); }
void video_pipeline_close(PPUContext& ctx, u32, u64) { later(ctx, "a video pipeline's close"); }

void register_video(Registry&) {}

} // namespace pspocket::hle::sailrec
