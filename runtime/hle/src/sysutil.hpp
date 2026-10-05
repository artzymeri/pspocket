#pragma once

// cellSysutil's callback queue, for other HLE modules (implemented in cellSysutil.cpp). The game registers up to four
// callbacks (cellSysutilRegisterCallback) and polls cellSysutilCheckCallback, usually once a frame; whatever is
// queued here runs inside that call, on the game's own thread. Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellSysutil.cpp
// (sysutil_send_system_cmd, sysutil_register_cb).

#include <functional>
#include <string_view>

#include "pspocket/core/types.hpp"
#include "pspocket/ppu/context.h"

namespace pspocket::hle {

// System event statuses (RPCS3 cellSysutil.h), the `status` of sysutil_send_event.
inline constexpr u64 CELL_SYSUTIL_REQUEST_EXITGAME = 0x0101;
inline constexpr u64 CELL_SYSUTIL_DRAWING_BEGIN = 0x0121;
inline constexpr u64 CELL_SYSUTIL_DRAWING_END = 0x0122;
inline constexpr u64 CELL_SYSUTIL_SYSTEM_MENU_OPEN = 0x0131;
inline constexpr u64 CELL_SYSUTIL_SYSTEM_MENU_CLOSE = 0x0132;
inline constexpr u64 CELL_SYSUTIL_BGMPLAYBACK_PLAY = 0x0141;
inline constexpr u64 CELL_SYSUTIL_BGMPLAYBACK_STOP = 0x0142;

// Queues a system event for every callback the game has registered right now; its next cellSysutilCheckCallback calls
// each one as callback(status, param, userdata). Returns the number of callbacks queued, CELL_SYSUTIL_ERROR_BUSY for
// a second DRAWING_BEGIN, or -1 for a DRAWING_END without a BEGIN (RPCS3 sysutil_send_system_cmd). 0 when no HLE
// process is running. Callable from any thread.
s32 sysutil_send_event(u64 status, u64 param);

// Queues a host function for the game's next cellSysutilCheckCallback, which runs it on the calling guest thread with
// that thread's context (RPCS3 sysutil_register_cb: dialogs call the game's callbacks this way, with call_guest). A
// non-zero result ends that check early and is its return value; the functions queued after it wait for the next
// check. Callable from any thread while an HLE process runs.
void sysutil_queue_call(std::function<s32(PPUContext& ctx)> fn);

// Who opened the message dialog (RPCS3 msg_dialog_source): cellMsgDialogAbort closes only the dialogs the game opened
// with cellMsgDialog's own functions.
enum class MsgDialogSource : u8 { CellMsgDialog, CellSaveData, CellGame, SceNp, SceNpTrophy };

// Opens the message dialog (RPCS3 cellMsgDialog.cpp open_msg_dialog, not blocking; implemented in cellSysutil.cpp).
// One at a time: while one is open the result is CELL_SYSUTIL_ERROR_BUSY. The dialog begins drawing (DRAWING_BEGIN to
// the game's callbacks; when a system dialog draws already, that event's CELL_SYSUTIL_ERROR_BUSY is the result and
// nothing opens) and stays open until it is closed. `callback` (a CellMsgDialogCallback descriptor, or 0) and
// `userdata` are for the user's answer. There is no dialog UI yet, so nothing answers a dialog opened here: only
// cellMsgDialogAbort closes one. (cellMsgDialogOpen2's own dialogs are answered at once: cellSysutil.cpp.)
s32 open_msg_dialog(MsgDialogSource source, u32 type, std::string_view message, u32 callback, u32 userdata);

} // namespace pspocket::hle
