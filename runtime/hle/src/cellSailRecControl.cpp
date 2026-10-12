// cellSailRec's control thread (D-918): libsail_rec's own thread that runs a booted recorder, its task handler thread
// and the internal functions it hands the game's objects (the feeders' and the composer's tables, the MP4 composer's
// file calls).

#include <cstring>
#include <format>

#include "cellSailRec.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle::sailrec {

void control_thread(PPUContext& ctx, u64) { later(ctx, "the control thread"); }

void task_thread(PPUContext& ctx, u64) { later(ctx, "the task handler thread"); }

namespace {

void _pspocket_cellSailRec_control(PPUContext& ctx, u64 rec) { control_thread(ctx, rec); }
void _pspocket_cellSailRec_task(PPUContext& ctx, u64 rec) { task_thread(ctx, rec); }

#define SAILREC_LATER(name, what) \
    s32 name(PPUContext& ctx) { later(ctx, what); }

SAILREC_LATER(_pspocket_cellSailRec_FsOpen, "the MP4 composer's file calls")
SAILREC_LATER(_pspocket_cellSailRec_FsClose, "the MP4 composer's file calls")
SAILREC_LATER(_pspocket_cellSailRec_FsFstat, "the MP4 composer's file calls")
SAILREC_LATER(_pspocket_cellSailRec_FsRead, "the MP4 composer's file calls")
SAILREC_LATER(_pspocket_cellSailRec_FsLseek, "the MP4 composer's file calls")
SAILREC_LATER(_pspocket_cellSailRec_FsNone, "the MP4 composer's file calls")
SAILREC_LATER(_pspocket_cellSailRec_FsWrite, "the MP4 composer's file calls")
SAILREC_LATER(_pspocket_cellSailRec_FsRename, "the MP4 composer's file calls")
SAILREC_LATER(_pspocket_cellSailRec_FsUnlink, "the MP4 composer's file calls")
SAILREC_LATER(_pspocket_cellSailRec_AudioMakeup, "an audio feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_AudioCleanup, "an audio feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_AudioOpen, "an audio feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_AudioStart, "an audio feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_AudioStop, "an audio feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_AudioCancel, "an audio feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_AudioCheckout, "an audio feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_AudioFormat, "an audio feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_AudioGet, "an audio feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_AudioRelease, "an audio feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_AudioClose, "an audio feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_VideoMakeup, "a video feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_VideoCleanup, "a video feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_VideoOpen, "a video feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_VideoStart, "a video feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_VideoStop, "a video feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_VideoCancel, "a video feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_VideoCheckout, "a video feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_VideoFormat, "a video feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_VideoGet, "a video feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_VideoRelease, "a video feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_VideoClose, "a video feeder's stream side")
SAILREC_LATER(_pspocket_cellSailRec_ComposerMakeup, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerCleanup, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerAlign, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerStreamSize, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerOpen, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerClose, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerAddAudio, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerAddVideo, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerAddUser, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerRemoveUser, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerRemoveVideo, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerRemoveAudio, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerStart, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerStop, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerEndAudio, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerEndVideo, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerEndUser, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerNone, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerTimescale, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerAudioAu, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerVideoAu, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerUserAu, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerCompleted, "the game's composer's table")
SAILREC_LATER(_pspocket_cellSailRec_ComposerFlushed, "the game's composer's table")

#undef SAILREC_LATER

} // namespace

void register_control(Registry& r)
{
    r.add(kChan, {
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_control"), _pspocket_cellSailRec_control),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_task"), _pspocket_cellSailRec_task),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_FsOpen"), _pspocket_cellSailRec_FsOpen),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_FsClose"), _pspocket_cellSailRec_FsClose),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_FsFstat"), _pspocket_cellSailRec_FsFstat),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_FsRead"), _pspocket_cellSailRec_FsRead),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_FsLseek"), _pspocket_cellSailRec_FsLseek),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_FsNone"), _pspocket_cellSailRec_FsNone),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_FsWrite"), _pspocket_cellSailRec_FsWrite),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_FsRename"), _pspocket_cellSailRec_FsRename),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_FsUnlink"), _pspocket_cellSailRec_FsUnlink),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_AudioMakeup"), _pspocket_cellSailRec_AudioMakeup),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_AudioCleanup"), _pspocket_cellSailRec_AudioCleanup),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_AudioOpen"), _pspocket_cellSailRec_AudioOpen),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_AudioStart"), _pspocket_cellSailRec_AudioStart),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_AudioStop"), _pspocket_cellSailRec_AudioStop),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_AudioCancel"), _pspocket_cellSailRec_AudioCancel),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_AudioCheckout"), _pspocket_cellSailRec_AudioCheckout),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_AudioFormat"), _pspocket_cellSailRec_AudioFormat),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_AudioGet"), _pspocket_cellSailRec_AudioGet),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_AudioRelease"), _pspocket_cellSailRec_AudioRelease),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_AudioClose"), _pspocket_cellSailRec_AudioClose),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_VideoMakeup"), _pspocket_cellSailRec_VideoMakeup),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_VideoCleanup"), _pspocket_cellSailRec_VideoCleanup),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_VideoOpen"), _pspocket_cellSailRec_VideoOpen),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_VideoStart"), _pspocket_cellSailRec_VideoStart),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_VideoStop"), _pspocket_cellSailRec_VideoStop),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_VideoCancel"), _pspocket_cellSailRec_VideoCancel),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_VideoCheckout"), _pspocket_cellSailRec_VideoCheckout),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_VideoFormat"), _pspocket_cellSailRec_VideoFormat),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_VideoGet"), _pspocket_cellSailRec_VideoGet),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_VideoRelease"), _pspocket_cellSailRec_VideoRelease),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_VideoClose"), _pspocket_cellSailRec_VideoClose),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerMakeup"), _pspocket_cellSailRec_ComposerMakeup),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerCleanup"), _pspocket_cellSailRec_ComposerCleanup),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerAlign"), _pspocket_cellSailRec_ComposerAlign),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerStreamSize"),
            _pspocket_cellSailRec_ComposerStreamSize),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerOpen"), _pspocket_cellSailRec_ComposerOpen),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerClose"), _pspocket_cellSailRec_ComposerClose),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerAddAudio"), _pspocket_cellSailRec_ComposerAddAudio),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerAddVideo"), _pspocket_cellSailRec_ComposerAddVideo),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerAddUser"), _pspocket_cellSailRec_ComposerAddUser),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerRemoveUser"),
            _pspocket_cellSailRec_ComposerRemoveUser),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerRemoveVideo"),
            _pspocket_cellSailRec_ComposerRemoveVideo),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerRemoveAudio"),
            _pspocket_cellSailRec_ComposerRemoveAudio),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerStart"), _pspocket_cellSailRec_ComposerStart),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerStop"), _pspocket_cellSailRec_ComposerStop),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerEndAudio"), _pspocket_cellSailRec_ComposerEndAudio),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerEndVideo"), _pspocket_cellSailRec_ComposerEndVideo),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerEndUser"), _pspocket_cellSailRec_ComposerEndUser),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerNone"), _pspocket_cellSailRec_ComposerNone),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerTimescale"),
            _pspocket_cellSailRec_ComposerTimescale),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerAudioAu"), _pspocket_cellSailRec_ComposerAudioAu),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerVideoAu"), _pspocket_cellSailRec_ComposerVideoAu),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerUserAu"), _pspocket_cellSailRec_ComposerUserAu),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerCompleted"),
            _pspocket_cellSailRec_ComposerCompleted),
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ComposerFlushed"), _pspocket_cellSailRec_ComposerFlushed),
    });
}

} // namespace pspocket::hle::sailrec
