// cellSailRec's feeders' stream side (D-918, part 7): the steps the recorder takes on a game's feeder (its open, start,
// stop and close, the frames it gives) as libsail_rec (fw 4.92) takes them. The later parts of D-918 make them.

#include "cellSailRec.hpp"
#include "pspocket/hle/hle.hpp"

namespace pspocket::hle::sailrec {

void feeder_audio_open(PPUContext& ctx, u32, u32, u32, u32, u64) { later(ctx, "an audio feeder's open"); }
void feeder_video_open(PPUContext& ctx, u32, u32, u32, u32, u64) { later(ctx, "a video feeder's open"); }
void feeder_audio_start(PPUContext& ctx, u32, u32, u64, u64, u64) { later(ctx, "an audio feeder's start"); }
void feeder_video_start(PPUContext& ctx, u32, u32, u64, u64, u64) { later(ctx, "a video feeder's start"); }
void feeder_audio_stop(PPUContext& ctx, u32, u32, u64, u64) { later(ctx, "an audio feeder's stop"); }
void feeder_video_stop(PPUContext& ctx, u32, u32, u64, u64) { later(ctx, "a video feeder's stop"); }
void feeder_audio_close(PPUContext& ctx, u32, u64) { later(ctx, "an audio feeder's close"); }
void feeder_video_close(PPUContext& ctx, u32, u64) { later(ctx, "a video feeder's close"); }

void register_feed(Registry&) {}

} // namespace pspocket::hle::sailrec
