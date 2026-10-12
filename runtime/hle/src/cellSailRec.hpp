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

// The feeders (0x200 bytes, the game's): the stream's side at +0 (eleven of libsail_rec's own callbacks, their
// argument the feeder, the recorder at +0x34, the index at +0x30, the state at +0x40), the game's callbacks at +0xC0
// (audio) or +0xD0 (video) and their argument at +0xEC or +0xFC.
namespace fd {
constexpr u32 kIndex = 0x30, kRecorder = 0x34, kState = 0x40, kAudioDiag = 0x98, kAudioDiagArg = 0x9C,
              kVideoDiag = 0xA0, kVideoDiagArg = 0xA4, kAudioFuncs = 0xC0, kAudioArg = 0xEC, kVideoFuncs = 0xD0,
              kVideoArg = 0xFC, kSize = 0x200;
}

// The game's composer (0x140 bytes): the recorder at +0, the game's 17 callbacks at +4, their argument at +0x48,
// libsail_rec's composer table at +0x4C, the state at +0x50, the type at +0x54, the streams, two locks ("_saimx0" at
// +0xB0, "_saimx1" at +0xD0, each an lwmutex and an lwcond).
namespace cp {
constexpr u32 kRecorder = 0, kFuncs = 4, kArg = 0x48, kTable = 0x4C, kState = 0x50, kType = 0x54, kAudioCount = 0x5C,
              kVideoCount = 0x60, kUserCount = 0x64, kAudio = 0x68, kVideo = 0x78, kUser = 0x88, kPending = 0xA0,
              kStreamParam = 0xA8, kLockA = 0xB0, kCondA = 0xC8, kLockB = 0xD0, kCondB = 0xE8, kDiag = 0xF4,
              kDiagArg = 0xF8, kSize = 0x140;
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

// A part of libsail_rec this part of D-918 does not make yet: the call site logged, the process stopped.
[[noreturn]] void later(PPUContext& ctx, std::string_view what);

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

} // namespace pspocket::hle::sailrec
