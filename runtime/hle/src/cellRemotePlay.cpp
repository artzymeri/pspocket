// cellRemotePlay: the Remote Play utility (libsysutil_remoteplay), through which a game being played on a PSP or a PS
// Vita over Remote Play learns so and adapts to it. pspocket's own as a console with no Remote Play peer (D-669): no
// PSP or PS Vita is registered with it (cellSubDisplay answers ZERO_REGISTERED, D-189), so no Remote Play session ever
// starts. Moved here from sysutil_misc.cpp with GetComparativeVolume, so that the library is one state.
//
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellRemotePlay.cpp (every function a todo answering CELL_OK; none takes an
// argument there but SetComparativeVolume's float and GetComparativeVolume's pointer, through which it writes 1.0) and
// cellRemotePlay.h (the status values, CELL_REMOTEPLAY_ERROR_INTERNAL); cellSubDisplay.h for
// CELL_REMOTEPLAY_ERROR_ZERO_REGISTERED (0x80029813, cellSubDisplay's ZERO_REGISTERED being that code). Nothing public
// describes the library past these, so each choice is recorded in D-669 with what to check against Sony's module:
// - What a session would give answers ZERO_REGISTERED, the code for this console's lack of a registered peer, writing
//   nothing: the peer's information (GetPeerInfo) and the memory shared with the session (GetSharedMemory). Their
//   arguments are not known and not read (RPCS3's CELL_OK with nothing written would hand a game what was in its own
//   buffer as a peer or a pointer).
// - GetStatus keeps RPCS3's answer, 0 (CELL_REMOTEPLAY_STATUS_LOADING's value): none of the six statuses names "no
//   session", and an error or another status could send a game down a path it does not take today.
// - The game's settings are kept and answer CELL_OK, as RPCS3's: the comparative volume (SetComparativeVolume, read back
//   by GetComparativeVolume; 1.0, RPCS3's value, until one is set), encrypting all the data sent to a peer
//   (EncryptAllData) and stopping the video sent to it (StopPeerVideoOut), for a session that never comes; Break ends
//   no session. An error for these could change a game's path for the worse, and a console with no session may well
//   take them alike.
// Each process its own library.

#include <format>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr const char* kChan = "cellRemotePlay";

enum : u32 {
    CELL_REMOTEPLAY_ERROR_ZERO_REGISTERED = 0x80029813, // RPCS3 cellSubDisplay.h
    CELL_REMOTEPLAY_ERROR_INTERNAL = 0x80029830,        // RPCS3 cellRemotePlay.h
};
constexpr s32 CELL_REMOTEPLAY_STATUS_LOADING = 0; // RPCS3 cellRemotePlay.h: LOADING 0 ... PREMOEND 5
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

struct State {
    std::mutex mutex;
    f32 volume = 1.0f;               // the comparative volume (RPCS3's 1.0 until the game sets one)
    bool encrypt_all = false;        // EncryptAllData asked for
    bool peer_video_stopped = false; // StopPeerVideoOut asked for
    std::set<std::string_view> logged; // the calls logged once
};

State& state() { return HleProcess::current()->state<State>(); }

// `r`, with the call logged the first time it is made: `what` says what this console makes of it.
s32 answer(std::string_view call, s32 r, std::string_view what)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (s.logged.insert(call).second)
        PS_LOG_INFO(kChan, "{}: {} -> {:#x}", call, what, static_cast<u32>(r));
    return r;
}

// int cellRemotePlayGetStatus(void): the status as the result. RPCS3's 0, kept (see above).
s32 cellRemotePlayGetStatus(PPUContext&)
{
    return answer("cellRemotePlayGetStatus", CELL_REMOTEPLAY_STATUS_LOADING,
        "no Remote Play session (no peer registered); RPCS3's answer, 0, kept");
}

// int cellRemotePlaySetComparativeVolume(float comparativeAudioVolume): kept as given, for GetComparativeVolume and a
// session that never comes (its range is not known: nothing is refused, as RPCS3 refuses nothing).
s32 cellRemotePlaySetComparativeVolume(PPUContext&, f32 volume)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    s.volume = volume;
    PS_LOG_INFO(kChan, "cellRemotePlaySetComparativeVolume({}): kept (no Remote Play session)", volume);
    return lv2::CELL_OK;
}

// int cellRemotePlayGetComparativeVolume(float* pComparativeAudioVolume): the volume kept, 1.0 until one is set; a null
// pointer gets nothing and CELL_OK (RPCS3), one that is not guest memory is logged with the call site and refused.
s32 cellRemotePlayGetComparativeVolume(PPUContext& ctx, u32 volume)
{
    if (!volume) return lv2::CELL_OK;
    if (!guest_readable(thread_of(ctx), volume, 4)) {
        log_call_site(ctx, guest_base(ctx),
            std::format("cellRemotePlayGetComparativeVolume: {:#x} is not guest memory", volume));
        return error(CELL_REMOTEPLAY_ERROR_INTERNAL);
    }
    State& s = state();
    std::lock_guard lock(s.mutex);
    ps_writef32(guest_base(ctx), volume, s.volume);
    return lv2::CELL_OK;
}

// What a session would give: none ever starts here. The arguments are not known and not read; nothing is written.
s32 cellRemotePlayGetPeerInfo(PPUContext&)
{
    return answer("cellRemotePlayGetPeerInfo", error(CELL_REMOTEPLAY_ERROR_ZERO_REGISTERED),
        "no peer (no PSP or PS Vita registered for Remote Play), nothing written");
}

s32 cellRemotePlayGetSharedMemory(PPUContext&)
{
    return answer("cellRemotePlayGetSharedMemory", error(CELL_REMOTEPLAY_ERROR_ZERO_REGISTERED),
        "no Remote Play session to share memory with (no peer registered), nothing written");
}

// The game's requests for a session, kept for one that never comes; CELL_OK as RPCS3.
s32 cellRemotePlayEncryptAllData(PPUContext&)
{
    {
        State& s = state();
        std::lock_guard lock(s.mutex);
        s.encrypt_all = true;
    }
    return answer("cellRemotePlayEncryptAllData", lv2::CELL_OK, "kept; no Remote Play session to encrypt");
}

s32 cellRemotePlayStopPeerVideoOut(PPUContext&)
{
    {
        State& s = state();
        std::lock_guard lock(s.mutex);
        s.peer_video_stopped = true;
    }
    return answer("cellRemotePlayStopPeerVideoOut", lv2::CELL_OK, "kept; no peer receives the video");
}

s32 cellRemotePlayBreak(PPUContext&)
{
    return answer("cellRemotePlayBreak", lv2::CELL_OK, "no Remote Play session to end");
}

} // namespace

void register_cellRemotePlay(Registry& r)
{
    r.add("cellRemotePlay", {
        // Made from RPCS3 without Sony's libsysutil_remoteplay to compare them with (D-594): D-669.
        PS_HLE_UNVERIFIED(0x533F41DF, cellRemotePlayGetStatus),
        PS_HLE_UNVERIFIED(0x743918BD, cellRemotePlaySetComparativeVolume),
        PS_HLE_UNVERIFIED(0xE12C8C19, cellRemotePlayGetComparativeVolume),
        PS_HLE_UNVERIFIED(0xA445CD55, cellRemotePlayGetPeerInfo),
        PS_HLE_UNVERIFIED(0xC267987B, cellRemotePlayGetSharedMemory),
        PS_HLE_UNVERIFIED(0xD29FE5E3, cellRemotePlayEncryptAllData),
        PS_HLE_UNVERIFIED(0xD6F3FC82, cellRemotePlayStopPeerVideoOut),
        PS_HLE_UNVERIFIED(0xFB793F27, cellRemotePlayBreak),
    });
}

} // namespace pspocket::hle
