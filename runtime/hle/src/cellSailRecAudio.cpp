// cellSailRec's audio pipeline (D-918, part 7): the frames of a game's audio feeder resampled, mixed down and encoded to
// AAC on the game's SPURS, as libsail_rec (fw 4.92) runs them. The later parts of D-918 make it.

#include "cellSailRec.hpp"
#include "pspocket/hle/hle.hpp"

namespace pspocket::hle::sailrec {

void audio_pipeline_start(PPUContext& ctx, u32, u64) { later(ctx, "an audio pipeline's start"); }
void audio_pipeline_stop(PPUContext& ctx, u32, u32, u64) { later(ctx, "an audio pipeline's stop"); }
void audio_pipeline_close(PPUContext& ctx, u32, u64) { later(ctx, "an audio pipeline's close"); }

void register_audio(Registry&) {}

} // namespace pspocket::hle::sailrec
