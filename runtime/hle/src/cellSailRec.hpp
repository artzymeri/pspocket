#pragma once

// cellSailRec's parts share these (D-918): the recorder's layout as libsail_rec.sprx (fw 4.92) keeps it in the game's
// memory, its message queue and lock, its events, the game's allocator, and the objects the game hands it.
//
// The recorder (0x2000 bytes, the game's; cleared by Initialize):
//   +0x400 .. +0x480 the continuation records of the stream's steps (open, close, stop)
//   +0x490 the game's allocator (CellSailMemAllocator: alloc and free descriptors, argument)
//   +0x4A0 the control thread's ID (u64)
//   +0x4A8 the message queue: capacity, count, ring, write and read indices, lv2 mutex and condition ("_saiqu0"),
//          laid out as libsail's (cellSail.hpp q::)
//   +0x4C8 the subscribed events (u64, all at first); +0x4D0 the event handler and +0x4D8 its argument
//   +0x4E0 the recorder's lwmutex and +0x4F8 its lwcond ("_saicx0")
//   +0x500 the command whose completion the game waits on, +0x508 the game's argument for its event (u64)
//   +0x510 the message the control thread handles (200 while it waits for one)
//   +0x514 1 while the task handler thread runs, +0x518 its ID (u64)
//   +0x580 a SPURS queue (CellSpursQueue) of 16 messages of 16 bytes at +0x600: the SPU tasks' messages
//   +0x700 1 once Boot's set-up is done; +0x704 the state
//   +0x708 the clock the pipelines share (+0x710 or none), +0x710 the recorder's own clock (0x58 bytes, "_saisr0")
//   +0x768 the stream's state; +0x770 the composers (0x34 bytes: four composer tables and arguments by type, the
//          instance, its streams, the type in use, the recorder); +0x7A4 the profile in use; +0x7A8 the file calls of
//          the MP4 composer (14 words: libsail_rec's own, or the game's)
//   +0x7E0 the attribute's mode; +0x7E4, +0x7E8, +0x7EC the audio, video and user stream counts; +0x7F0, +0x7F4,
//          +0x7F8 their slots (one block from the game's allocator, the queue's ring after them); +0x7FC the timescale
//   +0x828 the profiles made; +0x82C, +0x830 the common taskset's (8 SPU priorities); +0x838 the common taskset,
//   +0x83C the video converters made, +0x840 the taskset's users; +0x844 the MP4 composer
//   +0x848 the game's SPURS, +0x84C its SPU threads; +0x850 the control thread's priority and +0x854 its stack;
//   +0x858, +0x85C the task handler's; +0x860, +0x864 the MP4 composer's two settings
//   +0x868 1 with the game's composer, +0x86C the game's composer
//   +0x870 the control thread's stack top less 2000 (u64), +0x878 the highest stack pointer the event handler was
//          called with (u64)

#include <cstdint>
#include <initializer_list>
#include <string_view>

#include "cellSail.hpp"
#include "guest.hpp"
#include "pspocket/hle/hle.hpp"

namespace pspocket::hle::sailrec {

inline constexpr std::string_view kChan = "cellSailRec";

using sail::error;
using sail::gb;
using sail::r32;
using sail::r64;
using sail::r8;
using sail::sc;
using sail::w16;
using sail::w32;
using sail::w64;
using sail::w8;
using sail::CELL_SAIL_ERROR_CANCEL;
using sail::CELL_SAIL_ERROR_EMPTY;
using sail::CELL_SAIL_ERROR_FULLED;
using sail::CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE;
using sail::CELL_SAIL_ERROR_INVALID_ARG;
using sail::CELL_SAIL_ERROR_INVALID_STATE;
using sail::CELL_SAIL_ERROR_MEMORY;
using sail::CELL_SAIL_ERROR_UNSUPPORTED_STREAM;
using sail::CELL_SAIL_ERROR_USING;

inline u16 r16(PPUContext& ctx, u32 a) { return ps_read16(gb(ctx), a); }

// Arithmetic as the PPU does it: a division by zero answers 0 (divwu, divdu: the interpreter's, as RPCS3's).
inline u32 divu(u32 a, u32 b) { return b ? a / b : 0; }
inline u64 divu64(u64 a, u64 b) { return b ? a / b : 0; }
inline s32 divs(s32 a, s32 b) { return b == 0 || (a == INT32_MIN && b == -1) ? 0 : a / b; }
inline s64 divs64(s64 a, s64 b) { return b == 0 || (a == INT64_MIN && b == -1) ? 0 : a / b; }
inline u64 sx32(u32 v) { return static_cast<u64>(static_cast<s64>(static_cast<s32>(v))); }

namespace rc {
constexpr u32 kOpenRecord = 0x400, kOpenSteps = 0x410, kCloseRecord = 0x420, kStopRecord = 0x450,
              kStopSteps = 0x460, kAlloc = 0x490, kFree = 0x494, kAllocArg = 0x498, kThread = 0x4A0, kQueue = 0x4A8,
              kQueueMutex = 0x4BC, kEventMask = 0x4C8, kHandler = 0x4D0, kOpenArg = 0x4D4, kHandlerArg = 0x4D8,
              kLock = 0x4E0, kCond = 0x4F8, kCommand = 0x500, kCommandArg = 0x508, kMessage = 0x510,
              kTaskRunning = 0x514, kTaskThread = 0x518, kSpursQueue = 0x580, kSpursQueueBuffer = 0x600,
              kRunning = 0x700, kState = 0x704, kClockInUse = 0x708, kClock = 0x710, kStreamState = 0x768,
              kStreamArg = 0x76C, kComposers = 0x770, kProfile = 0x7A4, kFileCalls = 0x7A8, kMode = 0x7E0,
              kAudioCount = 0x7E4, kVideoCount = 0x7E8, kUserCount = 0x7EC, kAudio = 0x7F0, kVideo = 0x7F4,
              kUser = 0x7F8, kTimescale = 0x7FC, kStartTime = 0x808, kEndTime = 0x820, kProfiles = 0x828,
              kTasksetParam = 0x82C, kTasksetPriorities = 0x830, kTaskset = 0x838, kConverters = 0x83C,
              kTasksetUsers = 0x840, kMp4 = 0x844, kSpurs = 0x848, kSpuCount = 0x84C, kThreadPrio = 0x850,
              kThreadStack = 0x854, kTaskPrio = 0x858, kTaskStack = 0x85C, kMp4Param1 = 0x860, kMp4Param2 = 0x864,
              kComposerSet = 0x868, kComposer = 0x86C, kStackTop = 0x870, kStackHigh = 0x878, kSize = 0x2000,
              kDumpSize = 0x880;
constexpr u32 kAudioStride = 0x2B8, kVideoStride = 0x460, kUserStride = 0x28;
// The states (+0x704).
constexpr u32 kInitialized = 0, kBooting = 1, kBooted = 2, kOpening = 3, kOpen = 4, kStarting = 5, kStarted = 6,
              kStopping = 7, kClosing = 8, kFinalized = 9, kEnded = 10;
} // namespace rc

// An audio slot (0x2B8 bytes) and a video slot (0x460): a continuation record at +0, the feeder at +0x48, the stream's
// pipeline at +0x50, the composer's stream at +0x278 (audio) or +0x410 (video), the index, the stream's state byte,
// and the per-stream parameters (68 to 72 audio, 73 to 81 video).
namespace as {
constexpr u32 kFeeder = 0x48, kPipeline = 0x50, kComposerStream = 0x278, kIndex = 0x294, kState = 0x295,
              kP68 = 0x296, kResample = 0x297, kP69 = 0x298, kP70 = 0x2A0, kP71 = 0x2A8, kP72 = 0x2B0;
}
namespace vs {
constexpr u32 kFeeder = 0x48, kPipeline = 0x50, kConverter = 0x408, kComposerStream = 0x410, kIndex = 0x42C,
              kState = 0x42D, kP73 = 0x42E, kP76 = 0x430, kP77 = 0x434, kP79 = 0x438, kP74 = 0x440, kP75 = 0x448,
              kP80 = 0x450, kP81 = 0x458;
}

// A profile (one block from the game's allocator, the control thread's): +0 its state (1 free, 2 a stream uses it), +4
// OpenStream's a, +8 the path (0x421 bytes, its length at +0x42C), +0x430 OpenStream's b, +0x438 the composer type,
// +0x43C 1 while the stream parameters are the defaults, +0x440 the composer's delay, +0x444 the buffer size, +0x450
// the stream parameters, +0x550, +0x554, +0x558 the audio (0x108 bytes each), video (0x220) and user (0x18) parameters,
// +0x55C the recorder, +0x560 the next profile.
namespace pf {
constexpr u32 kState = 0, kPath = 8, kPathLen = 0x42C, kType = 0x438, kDefaults = 0x43C, kDelay = 0x440,
              kBuffer = 0x444, kBuffer2 = 0x448, kStream = 0x450, kAudio = 0x550, kVideo = 0x554, kUser = 0x558,
              kRecorder = 0x55C, kNext = 0x560;
}

// The feeders (0x200 bytes, the game's): the stream's side at +0 (eleven of libsail_rec's own callbacks, their
// argument the feeder at +0x2C, the index at +0x30, the recorder at +0x34, the pipeline and the slot at +0x38 and
// +0x3C, the state at +0x40, the session's result at +0x44, the frames to drain at +0x48, the frames the game has out
// at +0x50 and the pipeline holds at +0x54, the step's handle at +0x58, the recording's start and end times and the
// last frame's at +0x60, +0x68, +0x70, the session at +0x78), the diagnostic handlers, the format kept (+0xA8 audio,
// 0x18 bytes; +0xB0 video, 0x20), the game's callbacks at +0xC0 (audio) or +0xD0 (video) and their argument at +0xEC
// or +0xFC.
namespace fd {
constexpr u32 kArgument = 0x2C, kIndex = 0x30, kRecorder = 0x34, kPipeline = 0x38, kSlot = 0x3C, kState = 0x40,
              kResult = 0x44, kDrain = 0x48, kFrames = 0x50, kHeld = 0x54, kHandle = 0x58, kStart = 0x60, kEnd = 0x68,
              kLast = 0x70, kSession = 0x78, kAudioDiag = 0x98, kAudioDiagArg = 0x9C, kVideoDiag = 0xA0,
              kVideoDiagArg = 0xA4, kAudioFormat = 0xA8, kVideoFormat = 0xB0, kAudioFuncs = 0xC0, kAudioArg = 0xEC,
              kVideoFuncs = 0xD0, kVideoArg = 0xFC, kSize = 0x200;
// libsail_rec's own callbacks (the stream's side's table at +0), each in front of the game's of the same index.
constexpr u32 kMakeup = 0, kCleanup = 1, kOpen = 2, kClose = 3, kStartCall = 4, kStop = 5, kCancel = 6, kFormat = 7,
              kCheckout = 8, kCheckin = 9, kClear = 10;
}

// The game's composer (0x140 bytes): the recorder at +0, the game's 17 callbacks at +4, their argument at +0x48,
// libsail_rec's composer table at +0x4C, the state at +0x50 (1 closed, 2 open, 3 recording, 4 opening, 5 closing, 6
// starting, 7 stopping), the type at +0x54, the session at +0x58, the streams (counts, and four entries each), the stop's
// result (+0x98) and the streams still recording (+0x9C), the pending call's handle (+0xA0), the stream parameters
// (+0xA8), two locks ("_saimx0" at +0xB0 for audio, "_saimx1" at +0xD0 for video and user, each an lwmutex and an
// lwcond).
namespace cp {
constexpr u32 kRecorder = 0, kFuncs = 4, kArg = 0x48, kTable = 0x4C, kState = 0x50, kType = 0x54, kSession = 0x58,
              kAudioCount = 0x5C, kVideoCount = 0x60, kUserCount = 0x64, kAudio = 0x68, kVideo = 0x78, kUser = 0x88,
              kStopResult = 0x98, kRecording = 0x9C, kPending = 0xA0, kStreamParam = 0xA8, kLockA = 0xB0, kCondA = 0xC8,
              kLockB = 0xD0, kCondB = 0xE8, kDiag = 0xF4, kDiagArg = 0xF8, kSize = 0x140;
// The game's callbacks (each a descriptor at kFuncs + 4 * index, none when 0).
constexpr u32 kMakeup = 0, kCleanup = 1, kOpen = 2, kClose = 3, kStart = 4, kStop = 5, kAddAudio = 7, kAddVideo = 8,
              kAudioArrived = 9, kVideoArrived = 10, kEndAudio = 11, kEndVideo = 12;
}

// A stream the composer was given (the composer's stream object, from the size its table asks): the stream entry it
// belongs to, its index, its state (9 removed, 10 added, 11 ended, 12 recording, 13 ended with nothing left), the
// parameters the game reads (0x50 bytes at +0x18), its timescale, and its ring of access units (+0x6C the ring, +0x70
// its size, +0x74 the units in it, +0x78 where the next goes, +0x7C which is read next; the ring itself from +0x80).
namespace so {
constexpr u32 kEntry = 0, kIndex = 4, kState = 8, kParams = 0x18, kTimescale = 0x68, kRing = 0x6C, kRingSize = 0x70,
              kCount = 0x74, kWrite = 0x78, kRead = 0x7C, kRingStart = 0x80;
constexpr u32 kRemoved = 9, kAdded = 10, kEnded = 11, kRecording = 12, kDone = 13;
}

// The recorder's composers (+0x770, 0x34 bytes): the composer tables by type (four) and their arguments, the instance of
// the one in use (its open's allocation), the stream entries given to it (a list linked at +0x14) and their number, the
// type in use (-1 none), the recorder.
namespace cs {
constexpr u32 kTables = 0, kArgs = 0x10, kInstance = 0x20, kStreams = 0x24, kCount = 0x28, kType = 0x2C,
              kRecorder = 0x30, kSize = 0x34;
// A composer table's functions (each a descriptor at 4 * index).
constexpr u32 kMakeup = 0, kCleanup = 1, kInstanceSize = 2, kStreamSize = 3, kOpen = 4, kClose = 5, kAddAudio = 6,
              kAddVideo = 7, kAddUser = 8, kRemoveAudio = 9, kRemoveVideo = 10, kRemoveUser = 11, kStart = 12,
              kStop = 13, kEndAudio = 14, kEndVideo = 15, kEndUser = 16, kNothing = 17, kCapacity = 18, kAudioAu = 19,
              kVideoAu = 20, kUserAu = 21, kCompleted = 22, kReleased = 23;
}

// A stream entry (0x1C bytes in a slot: audio +0x278, video +0x410): the units the composer can still take, the
// composer's stream object, the pipeline, a flag, the next entry of the composers' list.
namespace se {
constexpr u32 kCapacity = 0, kObject = 8, kPipeline = 0xC, kFlag = 0x10, kNext = 0x14, kSize = 0x1C;
}

// A video converter (0x100 bytes from the game's allocator, after the conversion's work area): the next converter of
// the recorder's list (+0x83C), the kind (0: a YUV 4:2:2 coding to ARGB or RGBA, 1: raw Bayer to ARGB or RGBA, 2: raw
// Bayer to a 4:2:2 coding, each on an SPU task; 3: JPEG through the firmware's cellAvchatJpgDec), the two codings, the
// fill byte, the sub-kind, the recorder, the work area, the module it loaded, the callback and its argument, an lwmutex
// ("_sailf0") with two lwconds ("_sailf1": room, results), the queue of requests (capacity, queued, results, the
// indices) and of results, and the conversion's handle (its work area, or the JPEG decoder's handle).
namespace cv {
constexpr u32 kNext = 0, kKind = 4, kIn = 8, kOut = 0xC, kFill = 0x10, kSub = 0x14, kRecorder = 0x18, kWork = 0x1C,
              kModule = 0x20, kCallback = 0x24, kArg = 0x28, kLock = 0x30, kRoom = 0x48, kReady = 0x50, kBusy = 0x58,
              kCapacity = 0x5C, kQueued = 0x60, kResults = 0x64, kWrite = 0x68, kStart = 0x6C, kDone = 0x70,
              kRead = 0x74, kRequests = 0x78, kResultTable = 0xA8, kHandle = 0xB8, kSize = 0x100;
}

// ------------------------------------------------------------------------------------------------- shared helpers

// The game's allocator: alloc(arg, boundary, size), free(arg, boundary, pointer).
u32 alloc(PPUContext& ctx, u32 rec, u32 boundary, u32 size);
void dealloc(PPUContext& ctx, u32 rec, u32 boundary, u32 ptr);

// The message queue (libsail_rec's, laid out as libsail's): {code, 0, object, arg} at the tail; a call's completion
// {0, result, handle} (nothing for a handle of 0).
s32 post(PPUContext& ctx, u32 rec, u32 code, u32 object, u32 arg);
s32 post_completion(PPUContext& ctx, u32 rec, s32 result, u64 handle);

// The recorder's lwmutex.
s32 lock(PPUContext& ctx, u32 rec);
s32 unlock(PPUContext& ctx, u32 rec);

// The game's event handler(arg, major << 32 | minor, a, b), when there is one; the stack bookkeeping libsail_rec does
// around it (+0x878, and the handler's frame lowered below the stack top's last 2000 bytes).
void event(PPUContext& ctx, u32 rec, u32 major, u32 minor, u64 a, u64 b);

// libsail_rec's composer table for the game's composer (its +0x4C): 24 internal functions, made once per process.
u32 composer_table(PPUContext& ctx);

// libsail_rec's "modules are the process's" flag (its module start's: lv2's access control engine says the process is
// system software, program type 1 or 2): with it libsail_rec loads no modules through cellSysmodule.
u32 modules_flag(PPUContext& ctx);

// The control thread's entry and the task handler's (internal functions).
void control_thread(PPUContext& ctx, u64 rec);
void task_thread(PPUContext& ctx, u64 rec);

// The control thread's answer to a command (the answer stored, the caller signalled, the lwmutex released), and an
// error event ("error 0x%X" printed first), as libsail_rec makes them.
void reply(PPUContext& ctx, u32 rec, u32 record, s32 answer);
void error_event(PPUContext& ctx, u32 rec, s32 err);

// What libsail_rec.sprx holds that pspocket's own reads from the installed system software (D-918 part 5, as
// cellSpursJq's policy module, D-537): an SPU program's guest copy (made once per process; 0 without the module,
// logged) and bytes of its tables (false without the module).
u32 spu_program(PPUContext& ctx, u32 vaddr);
bool module_bytes(u32 vaddr, u32 size, u8* out);

// A firmware library's function the recorder calls (cellAvchatJpgDec, the encoders, cellMp4...): the export the
// loader linked for this process (Registry::add_imports); without one, logged and CELL_SAIL_ERROR_FATAL.
u64 firmware_call(PPUContext& ctx, std::string_view library, u32 nid, std::initializer_list<u64> args);

// libsail_rec's module loads (cellSysmodule; the Internal calls for IDs from 0xF000; nothing when the modules are the
// process's, modules_flag).
s32 load_module(PPUContext& ctx, u32 id);
s32 unload_module(PPUContext& ctx, u32 id);

// The common taskset ("_libsail-common-taskset", made at its first user, one more user each call; 0 when it cannot be
// made) and one user less (shut down, joined and freed after the last).
u32 common_taskset(PPUContext& ctx, u32 rec);
void release_taskset(PPUContext& ctx, u32 rec);

// Video converters (cellSailRecConvert.cpp): the control thread's CreateVideoConverter (message 3) and
// DestroyVideoConverter (4), the end's, a conversion's result from the task handler, the converter's notification
// (messages 0x32 to 0x35), and the next request started (Process, GetResult, a result).
void create_converter(PPUContext& ctx, u32 rec, u32 block, u32 record);
void destroy_converter(PPUContext& ctx, u32 rec, u32 converter, u32 record);
void destroy_converters(PPUContext& ctx, u32 rec);
void converter_done(PPUContext& ctx, u32 converter, u32 result);
void converter_notified(PPUContext& ctx, u32 converter);
s32 converter_start_next(PPUContext& ctx, u32 converter);

// ------------------------------------------------------------------------------------------------- D-918 part 6 on

// One of libsail_rec's internal functions (a registered internal function of cellSailRec) as a guest descriptor.
u32 internal(std::string_view name);

// sysPrxForUser's strncpy and strlen on guest memory, as libsail_rec calls them.
void guest_strncpy(PPUContext& ctx, u32 dst, u32 src, u32 n);
u32 guest_strlen(PPUContext& ctx, u32 s);

// The command the game waits on completed (event 2, when it is the one pending at +0x500).
void command_completed(PPUContext& ctx, u32 rec, u32 command, s32 result);

// The streams (cellSailRecStream.cpp): the control thread's messages 5 (OpenStream), 7 (CloseStream), 9 (Start), 0xB
// (Stop), 0xC (the composer's flushing stop), 0x2E (a composer's session error); everything closed or stopped; and
// Finalize's stop by the state.
void open_stream(PPUContext& ctx, u32 rec, u32 block, u32 record);
void close_stream(PPUContext& ctx, u32 rec, u32 block, u32 record);
void start(PPUContext& ctx, u32 rec, u32 block, u32 record);
void stop(PPUContext& ctx, u32 rec, u32 block, u32 record);
void stop_flush(PPUContext& ctx, u32 rec, u32 block, u32 record);
void composer_session_error(PPUContext& ctx, u32 rec, u32 err);
void close_all(PPUContext& ctx, u32 rec, u32 record, u64 parent);
void stop_all(PPUContext& ctx, u32 rec, u32 record, u32 flag, u64 parent);
void finalize_by_state(PPUContext& ctx, u32 rec);
s32 profile_composer_release(PPUContext& ctx, u32 rec, u32 profile);

// The composers (cellSailRecCompose.cpp; `cs` the recorder's +0x770): register and unregister a composer table by
// type, reset, and each step through the table of the composer in use (each ends with a completion for `handle`).
s32 composer_register(PPUContext& ctx, u32 cs, u32 type, u64 table, u32 arg);
s32 composer_unregister(PPUContext& ctx, u32 cs, u32 type, u32 table_out, u32 arg_out);
void composers_reset(PPUContext& ctx, u32 cs, u32 rec);
void composer_completed(PPUContext& ctx, u32 cs, s32 status);
void composer_open(PPUContext& ctx, u32 cs, u32 type, u32 path, u32 profile_type, u64 handle);
void composer_close(PPUContext& ctx, u32 cs, u64 handle);
void composer_remove(PPUContext& ctx, u32 cs, u32 entry, u64 handle, bool video);
void composer_free_stream(PPUContext& ctx, u32 cs, u32 entry);
void composer_detach_instance(PPUContext& ctx, u32 cs);
void composer_add(PPUContext& ctx, u32 cs, u32 entry, u32 params, u32 pipeline, u32 flag, u64 handle, bool video);
void composer_start(PPUContext& ctx, u32 cs, u32 path, u32 session, u64 handle);
void composer_stop(PPUContext& ctx, u32 cs, u32 flag, u64 handle);
void composer_end(PPUContext& ctx, u32 cs, u32 entry, u64 handle, bool video);
void composer_capacity(PPUContext& ctx, u32 cs, u32 entry);
void composer_released(PPUContext& ctx, u32 cs, u32 entry, u32 n);
s32 composer_au(PPUContext& ctx, u32 cs, u32 entry, u32 au, bool video);
// The game's composer: attached at OpenStream (its makeup, registered), detached at the close (unregistered, its
// cleanup), and cellSailComposerNotifyCallCompleted's work.
s32 composer_attach(PPUContext& ctx, u32 composer, u32 rec, u32 cs);
void composer_detach(PPUContext& ctx, u32 composer, u32 cs);
u64 composer_notify_completed(PPUContext& ctx, u64 composer, s32 result);
// The MP4 composer (type 1, libmp4's): made or counted at OpenStream with its modules loaded; destroyed by libmp4.
s32 mp4_composer_attach(PPUContext& ctx, u32 rec);
void mp4_composer_destroy(PPUContext& ctx, u32 rec, u32 mp4);

// The feeders (cellSailRecFeed.cpp): the stream's side of feeder `f` through libsail_rec's own table (makeup and
// cleanup at Boot and the end, Cancel's cancel); each step ends with a completion for `handle`. `video`: a video
// feeder (its notifications' codes and diagnostic handler).
// The name of libsail_rec's own callback `i` (an internal function) for an audio or video feeder.
std::string_view feeder_call_name(bool video, u32 i);
s32 feeder_makeup(PPUContext& ctx, u32 f, u32 rec, u32 index);
s32 feeder_cleanup(PPUContext& ctx, u32 f);
void feeder_cancel(PPUContext& ctx, u32 f);
void feeder_open(PPUContext& ctx, u32 f, u32 pipeline, u32 slot, u32 format, u64 handle, bool video);
void feeder_start(PPUContext& ctx, u32 f, u32 session, u64 start, u64 end, u64 handle, bool video);
void feeder_stop(PPUContext& ctx, u32 f, u32 flag, u64 end, u64 handle);
void feeder_close(PPUContext& ctx, u32 f, u64 handle);
// The format the feeder gives into `out` (its own callback 7).
s32 feeder_format(PPUContext& ctx, u32 f, u32 out);
// The game's call completed (messages 0xF, 0x19), frames out (0x10, 0x1A), whether frames wait, a frame taken for the
// pipeline into `out` (-1: given straight back), a frame the pipeline gives back.
void feeder_completed(PPUContext& ctx, u32 f, s32 result);
void feeder_frames_out(PPUContext& ctx, u32 f, u32 n);
bool feeder_has_frames(PPUContext& ctx, u32 f);
s32 feeder_take(PPUContext& ctx, u32 f, u32 out, bool video);
s32 feeder_release(PPUContext& ctx, u32 f, u32 frame);

// The pipelines (cellSailRecAudio.cpp, cellSailRecVideo.cpp; `p` a slot's +0x50): made at Boot (state 1) and taken
// down at the end; each step ends with a completion for `handle`.
void audio_pipeline_init(PPUContext& ctx, u32 p, u32 rec, u32 clock);
void audio_pipeline_fini(PPUContext& ctx, u32 p);
void audio_pipeline_start(PPUContext& ctx, u32 p, u64 handle);
void audio_pipeline_stop(PPUContext& ctx, u32 p, u32 flag, u64 handle);
void audio_pipeline_close(PPUContext& ctx, u32 p, u64 handle);
void video_pipeline_init(PPUContext& ctx, u32 p, u32 rec, u32 clock);
void video_pipeline_fini(PPUContext& ctx, u32 p);
void video_pipeline_start(PPUContext& ctx, u32 p, u32 slot, u32 params, u64 handle);
void video_pipeline_stop(PPUContext& ctx, u32 p, u32 flag, u64 handle);
void video_pipeline_close(PPUContext& ctx, u32 p, u64 handle);
// An audio stream configured once its feeder is open (the format it gives against the profile's), its pipeline opened
// and the stream added to the composer (two steps for `handle`): 2, or the error.
s32 audio_stream_open(PPUContext& ctx, u32 rec, u32 record, u64 handle);
// The control thread's messages for the audio streams (0x10, 0x11, 0x14 to 0x18, 0x2B; `a` and `b` the message's
// object and argument).
void audio_message(PPUContext& ctx, u32 rec, u32 code, u32 a, u32 b);
// A video stream configured once its feeder is open, its pipeline opened and the stream added to the composer (two
// steps for `handle`): 2, or the error.
s32 video_stream_open(PPUContext& ctx, u32 rec, u32 record, u64 handle);
// The control thread's messages for the video streams (0x1A, 0x1B, 0x1E to 0x23, 0x2C).
void video_message(PPUContext& ctx, u32 rec, u32 code, u32 a, u32 b);

// The recorder's clock (+0x710 in mode 0): reset at each Start; a stream's time `a` for the source's `b` (`c` its
// timescale), under the clock's lwmutex: the first stream to tell it owns it, later times from it smoothed.
void clock_reset(PPUContext& ctx, u32 clock);
void clock_update(PPUContext& ctx, u32 clock, u32 owner, u64 a, u64 b, u64 c);
// A video stream's time `a` (its frame's `b`, `c` its timescale) on the clock: the first stream to tell it sets it, its
// own times follow it as clock_update's; another stream's time mapped through the clock's times so far.
u64 clock_time(PPUContext& ctx, u32 clock, u32 owner, u64 a, u64 b, u64 c);

// A copy of libsail_rec's data (`size` bytes at `vaddr` of the installed module) in guest memory, made once per process
// (0 without the module, logged).
u32 module_copy(PPUContext& ctx, u32 vaddr, u32 size);

} // namespace pspocket::hle::sailrec
