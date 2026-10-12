// cellSailRec's video pipeline (D-918, part 8): the pictures of a game's video feeder converted, scaled and encoded on
// the game's SPURS, as libsail_rec (fw 4.92) runs them. The later parts of D-918 make the stream's configuration and
// the pipeline's steps; Boot's set-up and the end's are here.

#include <cstring>

#include "cellSailRec.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle::sailrec {

namespace {

// The pipeline (a slot's +0x50, 0x3C0 bytes): the recorder at +0, the clock at +0xC, the state at +0x168 (1 made).
constexpr u32 kSize = 0x3C0, kState = 0x168;

s32 _pspocket_cellSailRec_VideoFeederOpened(PPUContext& ctx) { later(ctx, "a video stream's configuration"); }

} // namespace

void video_pipeline_init(PPUContext& ctx, u32 p, u32 rec, u32 clock)
{
    std::memset(gb(ctx) + p, 0, kSize);
    w32(ctx, p + kState, 1);
    w32(ctx, p, rec);
    w32(ctx, p + 0xC, clock);
}

void video_pipeline_fini(PPUContext& ctx, u32 p)
{
    if (r32(ctx, p + kState) > 1) later(ctx, "a video pipeline's resources");
    std::memset(gb(ctx) + p, 0, kSize);
}

void video_pipeline_start(PPUContext& ctx, u32, u32, u32, u64) { later(ctx, "a video pipeline's start"); }
void video_pipeline_stop(PPUContext& ctx, u32, u32, u64) { later(ctx, "a video pipeline's stop"); }
void video_pipeline_close(PPUContext& ctx, u32, u64) { later(ctx, "a video pipeline's close"); }

#define SAILREC_INTERNAL(fn) PS_HLE_INTERNAL(recomp::nid_of(#fn), fn)

void register_video(Registry& r)
{
    r.add(kChan, {
        SAILREC_INTERNAL(_pspocket_cellSailRec_VideoFeederOpened),
    });
}

#undef SAILREC_INTERNAL

} // namespace pspocket::hle::sailrec
