// cellVideoPlayerUtility: the system software's video player, run inside a game or an application
// (libsysutil_video_player.sprx, internal module 0xF048, which only cellSysmoduleLoadModuleEx loads), pspocket's own as
// a console whose system software has no video player to lend (D-679). Unverified (D-594): made without Sony's module.
//
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellVideoPlayerUtility.cpp registers the seventeen names only (each
// UNIMPLEMENTED_FUNC answering CELL_OK, no arguments, no header), and nothing public describes the library: its names
// say a player opened, controlled and closed, its pictures and status read, positions and volume set, content still
// downloading (SetDownloadPosition, SetTransferComplete, GetTransferPictureInfo) and thumbnails made. So the model is
// the smallest one those names describe, every choice recorded in D-679 with what to check on the Mac:
// - Initialize and Finalize bracket the library, a process each: a second Initialize, and anything else before
//   Initialize or after Finalize, answers CELL_ESTAT.
// - There is no player: Open and StartThumbnail answer CELL_ENOTSUP, nothing opened or started, so that a caller falls
//   back to its own playback (or none) at once. RPCS3's CELL_OK with nothing written would leave a caller polling a
//   status that never changes and drawing pictures never written.
// - With nothing open, every call on the player (Close, PlaybackControl, the output pictures, the playback status, the
//   transfer picture information, the volume, the positions, the transfer's end) and EndThumbnail answer CELL_ESTAT.
// - Arguments: none is read. Their types are not known, and a pointer guessed wrong would be read, or written, in the
//   game's own data.
// - Error codes: the library's own are not known; these are lv2's (sys/return_code.h, rpcs3/Emu/Cell/ErrorCodes.h),
//   which the system libraries share, as cellLibprof's (D-638).
// Each refusal is logged once a function and reason, then at debug level (a player is polled every frame).

#include <format>
#include <mutex>
#include <set>
#include <string_view>
#include <utility>

#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kModule = "cellVideoPlayerUtility";
constexpr const char* kChan = "cellVideoPlayerUtility";

constexpr s32 error(lv2::CellError code) { return static_cast<s32>(code); }

struct State {
    std::mutex mutex;
    bool initialized = false;
    std::set<std::pair<std::string_view, std::string_view>> logged; // (function, reason) refusals logged once
};

State& state() { return HleProcess::current()->state<State>(); }

// A refusal, logged at warn the first time a function answers it for this reason, at debug after that.
s32 refuse(State& s, std::string_view call, lv2::CellError code, std::string_view why)
{
    if (s.logged.emplace(call, why).second) {
        PS_LOG_WARN(kChan, "{}: {} -> {:#010x}", call, why, static_cast<u32>(code));
    } else {
        PS_LOG_DEBUG(kChan, "{}: {} -> {:#010x}", call, why, static_cast<u32>(code));
    }
    return error(code);
}

// int cellVideoPlayerInitialize(...): the library up. Its arguments are not read (D-679).
s32 cellVideoPlayerInitialize(PPUContext&)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (s.initialized) return refuse(s, "cellVideoPlayerInitialize", lv2::CELL_ESTAT, "initialized already");
    s.initialized = true;
    PS_LOG_INFO(kChan, "cellVideoPlayerInitialize: up; there is no system video player here, so nothing will open");
    return lv2::CELL_OK;
}

// int cellVideoPlayerFinalize(...)
s32 cellVideoPlayerFinalize(PPUContext&)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return refuse(s, "cellVideoPlayerFinalize", lv2::CELL_ESTAT, "not initialized");
    s.initialized = false;
    PS_LOG_INFO(kChan, "cellVideoPlayerFinalize");
    return lv2::CELL_OK;
}

// What needs the system's player (opening content, making a thumbnail): not supported here, once initialized.
s32 no_player(std::string_view call)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return refuse(s, call, lv2::CELL_ESTAT, "not initialized");
    return refuse(s, call, lv2::CELL_ENOTSUP, "no system video player here, nothing opened");
}

// What needs content open (or a thumbnail started): never, since nothing opens here.
s32 nothing_open(std::string_view call)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    return refuse(s, call, lv2::CELL_ESTAT, s.initialized ? "nothing open" : "not initialized");
}

s32 cellVideoPlayerOpen(PPUContext&) { return no_player("cellVideoPlayerOpen"); }
s32 cellVideoPlayerStartThumbnail(PPUContext&) { return no_player("cellVideoPlayerStartThumbnail"); }

s32 cellVideoPlayerClose(PPUContext&) { return nothing_open("cellVideoPlayerClose"); }
s32 cellVideoPlayerEndThumbnail(PPUContext&) { return nothing_open("cellVideoPlayerEndThumbnail"); }
s32 cellVideoPlayerPlaybackControl(PPUContext&) { return nothing_open("cellVideoPlayerPlaybackControl"); }
s32 cellVideoPlayerGetOutputPicture(PPUContext&) { return nothing_open("cellVideoPlayerGetOutputPicture"); }
s32 cellVideoPlayerGetOutputStereoPicture(PPUContext&) { return nothing_open("cellVideoPlayerGetOutputStereoPicture"); }
s32 cellVideoPlayerGetPlaybackStatus(PPUContext&) { return nothing_open("cellVideoPlayerGetPlaybackStatus"); }
s32 cellVideoPlayerGetTransferPictureInfo(PPUContext&) { return nothing_open("cellVideoPlayerGetTransferPictureInfo"); }
s32 cellVideoPlayerGetVolume(PPUContext&) { return nothing_open("cellVideoPlayerGetVolume"); }
s32 cellVideoPlayerSetVolume(PPUContext&) { return nothing_open("cellVideoPlayerSetVolume"); }
s32 cellVideoPlayerSetDownloadPosition(PPUContext&) { return nothing_open("cellVideoPlayerSetDownloadPosition"); }
s32 cellVideoPlayerSetStartPosition(PPUContext&) { return nothing_open("cellVideoPlayerSetStartPosition"); }
s32 cellVideoPlayerSetStopPosition(PPUContext&) { return nothing_open("cellVideoPlayerSetStopPosition"); }
s32 cellVideoPlayerSetTransferComplete(PPUContext&) { return nothing_open("cellVideoPlayerSetTransferComplete"); }

} // namespace

void register_cellVideoPlayerUtility(Registry& r)
{
    r.add(kModule, {
        PS_HLE_UNVERIFIED(0x4B77752F, cellVideoPlayerInitialize),
        PS_HLE_UNVERIFIED(0x72440039, cellVideoPlayerFinalize),
        PS_HLE_UNVERIFIED(0xB5542E5A, cellVideoPlayerOpen),
        PS_HLE_UNVERIFIED(0x801DB596, cellVideoPlayerClose),
        PS_HLE_UNVERIFIED(0xF16D10CB, cellVideoPlayerPlaybackControl),
        PS_HLE_UNVERIFIED(0xDF07BE79, cellVideoPlayerGetOutputPicture),
        PS_HLE_UNVERIFIED(0xD9C63AFB, cellVideoPlayerGetOutputStereoPicture),
        PS_HLE_UNVERIFIED(0xDC9462C5, cellVideoPlayerGetPlaybackStatus),
        PS_HLE_UNVERIFIED(0x8528C19A, cellVideoPlayerGetTransferPictureInfo),
        PS_HLE_UNVERIFIED(0x69452CBA, cellVideoPlayerGetVolume),
        PS_HLE_UNVERIFIED(0xC50303B2, cellVideoPlayerSetVolume),
        PS_HLE_UNVERIFIED(0x8CBDCBAC, cellVideoPlayerSetDownloadPosition),
        PS_HLE_UNVERIFIED(0x5C1872F1, cellVideoPlayerSetStartPosition),
        PS_HLE_UNVERIFIED(0x7C4BA244, cellVideoPlayerSetStopPosition),
        PS_HLE_UNVERIFIED(0xDD3D7EC0, cellVideoPlayerSetTransferComplete),
        PS_HLE_UNVERIFIED(0xA2B8AF7C, cellVideoPlayerStartThumbnail),
        PS_HLE_UNVERIFIED(0xAFAFEDB1, cellVideoPlayerEndThumbnail),
    });
}

} // namespace pspocket::hle
