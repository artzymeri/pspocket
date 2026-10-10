// cellSail's built-in file source (D-901): the source libsail (fw 4.92) registers for the "x-cell-fs" protocol in every
// player, read from libsail's code and compared with it running on pspocket (tests/hle/test_sail_source.cpp).
//
// The source is 0x138 bytes in the player's block: its reader's state, the CellSailSource it registers (+0x40, its
// argument the source itself) and its file calls (+0x100: -1, open, a call after open (none), close, fstat, read, lseek,
// cancel, cellFs's own or the game's, player parameter 33). A URI is "x-cell-fs://" and the path: the source opens
// the URI's 13th character on, whatever comes before it.
//
// - Synchronously (DescriptorOpen's header): ReadSync opens the file, stats it for its size, seeks, reads and closes it.
// - Asynchronously (playing): Makeup loads cellFs (cellSysmodule 0xE) and starts a reader thread ("_libsail-source_fs",
//   the player's file source priority and stack, 1200 and 4 KB when 0) that Cleanup stops and joins. The callbacks
//   hand the thread one command at a time (a byte, under the source's lwmutex "_saifs0", with its lwcond): Open (0; the
//   path kept, a 2 MB read-ahead buffer from the player's allocator, two items of 1 MB), Start (1; a session from a
//   position for a length), Checkin (2), Stop (4), Close (5), Read (7, one read into the game's buffer), Clear (8), the
//   end (10). The thread is in one of three states: closed (1), open (2), started (3). Started, it fills the two items
//   one after the other while one is free and tells the player (StreamOut); at the length's end or the file's, InputEos
//   and back to open. Its other answers are CallCompleted; an open file is polled each second (its medium gone: media
//   state 2), a failed read retried after half a second (media state 1 and a session error) or the file opened again
//   after five.
//
// Where libsail's code takes stack variables (the descriptor, read count and seek position it hands the file calls, the
// reader's path and Read's arguments, which it keeps in the source) pspocket takes them at the same addresses, from a
// frame of libsail's size (Frame): a game's file calls see what libsail would hand them.

#include <cstring>
#include <format>

#include "cellSail.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace sail {
namespace {

// The source's fields.
namespace fs {
constexpr u32 kCommand = 0, kReady = 1, kTake = 2, kFill = 3, kState = 4, kFd = 8, kThread = 0x10, kPath = 0x18,
              kPathChanged = 0x1C, kItems = 0x20, kSource = fsrc::kSource, kMutex = 0xC0, kCond = 0xD8, kChunk = 0xE0,
              kLength = 0xE8, kStart = 0xF0, kSession = 0xF8, kReadArgs = 0xFC, kCalls = fsrc::kCalls;
constexpr u32 kItemSize = 16, kBufferSize = 0x200000, kChunkSize = 0x100000;
} // namespace fs
// The reader writes the items ready, the descriptor and (Clear) the next item out without the lock, and reads the items'
// buffers without it, as libsail does; pspocket makes those accesses atomic on both sides (D-585).

// The file calls, by their word in the source's +0x100.
enum FileCall : u32 { kOpen = 1, kAfterOpen = 2, kClose = 3, kFstat = 4, kRead = 5, kLseek = 6, kCancel = 7 };

// The reader's commands (the source's first byte; -1 none). 3, 6 and 9 the thread gives itself.
enum Command : s32 {
    kNone = -1, kCmdOpen = 0, kCmdStart = 1, kCmdCheckin = 2, kCmdFill = 3, kCmdStop = 4, kCmdClose = 5, kCmdReopen = 6,
    kCmdRead = 7, kCmdClear = 8, kCmdPoll = 9, kCmdEnd = 10,
};

// The reader's states.
enum State : u32 { kEnded = 0, kClosed = 1, kOpened = 2, kStarted = 3 };

constexpr u64 kSecond = 1000000;

// The internal functions libsail hands out by descriptor.
u32 internal(std::string_view name) { return HleProcess::current()->link_function("cellSail", recomp::nid_of(name), name); }

u64 sx(s32 v) { return static_cast<u64>(static_cast<s64>(v)); }

// A stack frame as libsail's function makes it (stdu r1,-size(r1)), so its local variables sit where libsail's do.
class Frame {
public:
    Frame(PPUContext& ctx, u32 size) : ctx_(ctx), saved_(ctx.gpr[1])
    {
        ctx.gpr[1] = saved_ - size;
        at_ = static_cast<u32>(ctx.gpr[1]);
        w64(ctx, at_, saved_);
    }
    ~Frame() { ctx_.gpr[1] = saved_; }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    u32 at(u32 offset) const { return at_ + offset; }

private:
    PPUContext& ctx_;
    const u64 saved_;
    u32 at_ = 0;
};

// Where libsail would fault on the game's pointer: the call site logged, the call refused.
bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, gb(ctx), std::format("cellSail: the x-cell-fs source's {} at {:#x} is not guest memory (libsail "
        "faults there); refused", what, addr));
    return false;
}

// ------------------------------------------------------------------------------------------------- libsail's helpers

// Whether libsail loads the modules it needs (its module_start asks lv2 whether the process is system software, as
// cellSailControl.cpp's loads_modules does): cellSysmodule's load and unload of `id`, or nothing.
bool loads_modules(PPUContext& ctx)
{
    const u32 pid = static_cast<u32>(sc(ctx, 1, {}));
    StackVar v(ctx, gb(ctx), 8);
    if (sc(ctx, 871, {1, pid, v.addr()}) != 0) return true;
    return (r64(ctx, v.addr()) & 0xFFFFFF) - 1 > 1;
}

s32 load_module(PPUContext& ctx, u32 id)
{
    if (!loads_modules(ctx)) return lv2::CELL_OK;
    return static_cast<s32>(module_call(ctx, "cellSysmodule", "cellSysmoduleLoadModule", {id}));
}

s32 unload_module(PPUContext& ctx, u32 id)
{
    if (!loads_modules(ctx)) return lv2::CELL_OK;
    return static_cast<s32>(module_call(ctx, "cellSysmodule", "cellSysmoduleUnloadModule", {id}));
}

// The source's lwmutex and lwcond (sysPrxForUser's).
s32 lock(PPUContext& ctx, u32 s) { return static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_lock", {s + fs::kMutex, 0})); }
s32 unlock(PPUContext& ctx, u32 s) { return static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_unlock", {s + fs::kMutex})); }
s32 wait(PPUContext& ctx, u32 s, u64 timeout)
{
    return static_cast<s32>(lv2_lib(ctx, "sys_lwcond_wait", {s + fs::kCond, timeout}));
}
s32 signal(PPUContext& ctx, u32 s) { return static_cast<s32>(lv2_lib(ctx, "sys_lwcond_signal", {s + fs::kCond})); }

// A command for the reader (the lock held): once the last one is taken, this one set and the reader woken.
s32 post_command(PPUContext& ctx, u32 s, Command c)
{
    for (;;) {
        if (r8(ctx, s + fs::kCommand) == 0xFF) {
            w8(ctx, s + fs::kCommand, static_cast<u8>(c));
            return signal(ctx, s);
        }
        if (const s32 r = wait(ctx, s, 0); r < 0) return r;
    }
}

// Waits (the lock held) until the reader has taken the last command.
s32 wait_taken(PPUContext& ctx, u32 s)
{
    while (r8(ctx, s + fs::kCommand) != 0xFF)
        if (const s32 r = wait(ctx, s, 0); r < 0) return r;
    return lv2::CELL_OK;
}

// One of the file calls (the source's +0x100 words: libsail's own or the game's), its answer's low word.
s32 file_call(PPUContext& ctx, u32 s, FileCall which, std::initializer_list<u64> args)
{
    return static_cast<s32>(call_guest(ctx, r32(ctx, s + fs::kCalls + 4 * which), args));
}

// The notifications, the CellSailSource's (cellSail.cpp's).
void completed(PPUContext& ctx, u32 s, s32 r)
{
    module_call(ctx, "cellSail", "cellSailSourceNotifyCallCompleted", {s + fs::kSource, sx(r)});
}
void session_error(PPUContext& ctx, u32 s, s32 r, u8 terminated)
{
    module_call(ctx, "cellSail", "cellSailSourceNotifySessionError", {s + fs::kSource, sx(r), terminated});
}
void media_state(PPUContext& ctx, u32 s, s32 state)
{
    module_call(ctx, "cellSail", "cellSailSourceNotifyMediaStateChanged", {s + fs::kSource, sx(state)});
}
void stream_out(PPUContext& ctx, u32 s) { module_call(ctx, "cellSail", "cellSailSourceNotifyStreamOut", {s + fs::kSource, 1}); }
void input_eos(PPUContext& ctx, u32 s) { module_call(ctx, "cellSail", "cellSailSourceNotifyInputEos", {s + fs::kSource}); }

// The read-ahead buffer back to the player's allocator (free(arg, 0x80, buffer)).
void free_buffer(PPUContext& ctx, u32 s)
{
    if (const u32 buf = atomic_load_be32(gb(ctx), s + fs::kItems)) {
        const u32 p = r32(ctx, s + fs::kSource);
        call_guest(ctx, r32(ctx, p + pl::kFree), {r32(ctx, p + pl::kAllocArg), 0x80, buf});
    }
    atomic_store_be32(gb(ctx), s + fs::kItems + fs::kItemSize, 0);
    atomic_store_be32(gb(ctx), s + fs::kItems, 0);
}

// Opens `path` read-only through the open call ({path, 0, &fd, 0, 0}), then the call after it when there is one
// ({path, 0, fd, 0, 0}), and keeps the descriptor at `out`. A descriptor of 0 is CELL_SAIL_ERROR_INVALID_FD; when the
// call after open fails, the file stays open (libsail's).
s32 open_file(PPUContext& ctx, u32 s, u32 path, u32 out)
{
    Frame f(ctx, 160);
    const u32 fdv = f.at(112);
    w32(ctx, fdv, 0);
    s32 r = file_call(ctx, s, kOpen, {path, 0, fdv, 0, 0});
    if (r < 0) return r;
    u32 fd = r32(ctx, fdv);
    if (!fd) return error(CELL_SAIL_ERROR_INVALID_FD);
    if (r32(ctx, s + fs::kCalls + 4 * kAfterOpen)) {
        r = file_call(ctx, s, kAfterOpen, {path, 0, sx(static_cast<s32>(fd)), 0, 0});
        if (r < 0) return r;
        fd = r32(ctx, fdv);
    }
    atomic_store_be32(gb(ctx), out, fd);
    return r;
}

// The URI's file, read once: opened, its size to `total` (when given and fstat answers), `size` bytes from `offset`
// into `buf`. The file is closed and the read's answer returned, unless `keep` and all of it was read: then the open
// descriptor is the answer. A short read answers the read's 0.
s32 read_once(PPUContext& ctx, u32 s, u32 uri, u64 offset, u32 buf, u32 size, u32 total, bool keep)
{
    if (!usable(ctx, uri + 12, 1, "URI") || (total && !usable(ctx, total, 8, "size's u64")))
        return error(CELL_SAIL_ERROR_INVALID_ARG);
    Frame f(ctx, 256);
    const u32 fdv = f.at(112), nread = f.at(120), pos = f.at(128), stat = f.at(136);
    const s32 opened = open_file(ctx, s, uri + 12, fdv);
    if (opened < 0) return opened;
    const u32 fd = r32(ctx, fdv);
    const u64 fdx = sx(static_cast<s32>(fd));
    // CellFsStat's st_size at +36 (the structure is packed).
    if (total && file_call(ctx, s, kFstat, {fdx, stat}) >= 0)
        w64(ctx, total, u64{r32(ctx, stat + 36)} << 32 | r32(ctx, stat + 40));
    s32 r = opened;
    if (offset) r = file_call(ctx, s, kLseek, {fdx, offset, 0, pos});
    if (r >= 0) {
        r = file_call(ctx, s, kRead, {fdx, buf, size, nread});
        if (r >= 0 && r64(ctx, nread) == size && keep) return static_cast<s32>(fd);
    }
    file_call(ctx, s, kClose, {fdx});
    return r;
}

// ------------------------------------------------------------------------------------------------- the reader thread

// libsail's reader: its frame keeps the path (+208, 287 bytes) and Read's arguments (+120: URI, media info, u64 offset,
// buffer, size, u64* total size); +112 the read count or seek position, +152 a CellFsStat.
void _pspocket_cellSail_srcThread(PPUContext& ctx, u64 arg)
{
    const u32 s = static_cast<u32>(arg);
    Frame f(ctx, 608);
    const u32 args = f.at(120), path = f.at(208), var = f.at(112), stat = f.at(152);
    w32(ctx, s + fs::kReadArgs, args);
    w32(ctx, s + fs::kState, kClosed);
    w32(ctx, s + fs::kPath, path);
    w8(ctx, s + fs::kCommand, 0xFF);
    const auto fd = [&] { return atomic_load_be32(gb(ctx), s + fs::kFd); };
    const auto fdx = [&] { return sx(static_cast<s32>(fd())); };
    const auto close_fd = [&] {
        const s32 r = file_call(ctx, s, kClose, {fdx()});
        atomic_store_be32(gb(ctx), s + fs::kFd, 0);
        return r;
    };
    u64 timeout = 0, remaining = 0, position = 0, start = 0;
    u32 session = 0;
    s32 r = lock(ctx, s);
    if (r >= 0) r = signal(ctx, s);
    if (r >= 0) {
        unlock(ctx, s);
        for (;;) {
            const u32 state = r32(ctx, s + fs::kState);
            r = lock(ctx, s);
            if (r < 0) break;
            s32 cmd = static_cast<s8>(r8(ctx, s + fs::kCommand));
            if (cmd == kNone) {
                if (state == kStarted && timeout == 0 && static_cast<s8>(atomic_load_u8(gb(ctx), s + fs::kReady)) <= 1) {
                    cmd = kCmdFill;
                } else {
                    for (;;) {
                        r = wait(ctx, s, timeout);
                        if (r < 0 && static_cast<u32>(r) != lv2::CELL_ETIMEDOUT) break;
                        cmd = static_cast<s8>(r8(ctx, s + fs::kCommand));
                        if (cmd != kNone || timeout == 0) {
                            if (cmd != kNone) break;
                            continue;
                        }
                        // Woken by the timeout (or without a command): started, read on or open the file again;
                        // else poll the open file.
                        if (state == kStarted) {
                            if (!fd()) {
                                cmd = kCmdReopen;
                            } else {
                                timeout = 0;
                                cmd = kCmdFill;
                            }
                        } else {
                            cmd = kCmdPoll;
                        }
                        break;
                    }
                    if (r < 0 && static_cast<u32>(r) != lv2::CELL_ETIMEDOUT) break;
                }
            }
            w8(ctx, s + fs::kCommand, 0xFF);
            if (cmd == kCmdOpen) {
                // Open copies the path (marking it changed) while the reader waits.
                w8(ctx, s + fs::kPathChanged, 0);
                if (signal(ctx, s) >= 0) wait(ctx, s, 0);
            } else if (cmd == kCmdStart) {
                session = r32(ctx, s + fs::kSession);
                start = r64(ctx, s + fs::kStart);
                w32(ctx, s + fs::kSession, 0);
                remaining = r64(ctx, s + fs::kLength);
                signal(ctx, s);
            } else if (cmd == kCmdRead) {
                // Read writes its arguments while the reader waits.
                if (signal(ctx, s) >= 0) wait(ctx, s, 0);
            }
            r = signal(ctx, s);
            unlock(ctx, s);
            if (r < 0) break;

            if (cmd == kCmdEnd) {
                w32(ctx, s + fs::kState, kEnded);
                r = 0;
                break;
            }
            if (cmd == kCmdClear) {
                atomic_store_u8(gb(ctx), s + fs::kTake, 0);
                w8(ctx, s + fs::kFill, 0);
                atomic_store_u8(gb(ctx), s + fs::kReady, 0);
                continue;
            }
            if (cmd == kCmdCheckin) atomic_store_u8(gb(ctx), s + fs::kReady, static_cast<u8>(atomic_load_u8(gb(ctx), s + fs::kReady) - 1));
            if (cmd == kCmdRead) {
                // In the closed state the file read stays open for an Open of the same path (the position after it:
                // the size read, not the offset and the size).
                const bool keep = state == kClosed;
                if (keep && fd()) close_fd();
                const u32 uri = r32(ctx, args), size = r32(ctx, args + 20);
                const s32 got = read_once(ctx, s, uri, r64(ctx, args + 8), r32(ctx, args + 16), size,
                    r32(ctx, args + 24), keep);
                s32 answer = got;
                if (got >= 0 && keep) {
                    lv2_lib(ctx, "_sys_strncpy", {path, uri + 12, 287});
                    w8(ctx, path + 286, 0);
                    atomic_store_be32(gb(ctx), s + fs::kFd, static_cast<u32>(got));
                    answer = 0;
                    position = size;
                }
                completed(ctx, s, answer);
                continue;
            }

            if (state == kOpened) {
                switch (cmd) {
                case kCmdOpen: {
                    if (r8(ctx, path) == 0) break;
                    if (fd()) {
                        if (r8(ctx, s + fs::kPathChanged) == 0) {
                            completed(ctx, s, 0);
                            break;
                        }
                        file_call(ctx, s, kClose, {fdx(), 0});
                        atomic_store_be32(gb(ctx), s + fs::kFd, 0);
                    }
                    position = 0;
                    completed(ctx, s, open_file(ctx, s, path, s + fs::kFd));
                    break;
                }
                case kCmdStart:
                    timeout = 5 * kSecond;
                    if (fd()) {
                        if (position == start) {
                            timeout = 0;
                        } else if (const s32 e = file_call(ctx, s, kLseek, {fdx(), start, 0, var}); e < 0) {
                            timeout = 5 * kSecond;
                            session_error(ctx, s, e, 0);
                        } else {
                            timeout = 0;
                        }
                    }
                    position = start;
                    completed(ctx, s, 0);
                    w32(ctx, s + fs::kState, kStarted);
                    break;
                case kCmdStop: completed(ctx, s, 1); break;
                case kCmdClose:
                    // With no file open, the answer is the last lwcond signal's.
                    if (fd()) r = close_fd();
                    w32(ctx, s + fs::kState, kClosed);
                    timeout = 0;
                    completed(ctx, s, r);
                    break;
                default:
                    // The poll: the file opened again, or its medium checked.
                    if (!fd()) {
                        if (open_file(ctx, s, r32(ctx, s + fs::kPath), s + fs::kFd) >= 0) {
                            timeout = kSecond;
                            media_state(ctx, s, 0);
                        }
                    } else if (static_cast<u32>(file_call(ctx, s, kFstat, {fdx(), stat})) == lv2::CELL_EBADF) {
                        media_state(ctx, s, 2);
                        timeout = 5 * kSecond;
                        close_fd();
                    }
                    break;
                }
                continue;
            }

            if (state == kClosed) {
                if (cmd == kCmdClose) {
                    completed(ctx, s, 1);
                } else if (cmd == kCmdOpen && r8(ctx, path) != 0) {
                    s32 opened = 0;
                    if (!fd() || r8(ctx, s + fs::kPathChanged) != 0) {
                        if (fd()) {
                            file_call(ctx, s, kClose, {fdx(), 0});
                            atomic_store_be32(gb(ctx), s + fs::kFd, 0);
                        }
                        position = 0;
                        opened = open_file(ctx, s, path, s + fs::kFd);
                    }
                    if (opened >= 0) {
                        timeout = kSecond;
                        w32(ctx, s + fs::kState, kOpened);
                    }
                    completed(ctx, s, opened);
                }
                continue;
            }

            if (state != kStarted) continue;
            switch (cmd) {
            case kCmdStop:
                w32(ctx, s + fs::kState, kOpened);
                timeout = kSecond;
                completed(ctx, s, 0);
                break;
            case kCmdClose:
                if (fd()) r = close_fd();
                completed(ctx, s, r);
                timeout = 0;
                w32(ctx, s + fs::kState, kClosed);
                break;
            case kCmdReopen: {
                const s32 e = open_file(ctx, s, r32(ctx, s + fs::kPath), s + fs::kFd);
                if (static_cast<u32>(e) == lv2::CELL_ENOTMOUNTED) {
                    atomic_store_be32(gb(ctx), s + fs::kFd, 0);
                } else if (e < 0) {
                    session_error(ctx, s, e, 1);
                    timeout = 0;
                } else if (const s32 e2 = file_call(ctx, s, kLseek, {fdx(), position, 0, var}); e2 < 0) {
                    close_fd();
                    session_error(ctx, s, e2, 1);
                    timeout = 0;
                } else {
                    timeout = 0;
                    media_state(ctx, s, 0);
                }
                break;
            }
            case kCmdStart: {
                const s32 e = file_call(ctx, s, kLseek, {fdx(), start, 0, var});
                if (e >= 0) position = start;
                completed(ctx, s, e);
                break;
            }
            default: {
                // Fill the next item while one is free.
                if (static_cast<s8>(atomic_load_u8(gb(ctx), s + fs::kReady)) > 1) break;
                const u64 chunk = r32(ctx, s + fs::kChunk);
                const u64 n = remaining <= chunk ? remaining : chunk;
                u64 got = 0;
                if (n) {
                    w64(ctx, var, 0);
                    const u32 item = s + fs::kItems + fs::kItemSize * static_cast<s8>(r8(ctx, s + fs::kFill));
                    const s32 e = file_call(ctx, s, kRead, {fdx(), atomic_load_be32(gb(ctx), item), n, var});
                    if (e < 0) {
                        if (static_cast<u32>(e) == lv2::CELL_EIO &&
                            static_cast<u32>(file_call(ctx, s, kFstat, {fdx(), stat})) == lv2::CELL_EBADF) {
                            media_state(ctx, s, 2);
                            timeout = 5 * kSecond;
                            close_fd();
                        } else {
                            timeout = kSecond / 2;
                            media_state(ctx, s, 1);
                        }
                        session_error(ctx, s, e, 0);
                        break;
                    }
                    got = r64(ctx, var);
                    if (got) {
                        position += got;
                        remaining -= got;
                        w32(ctx, item + 4, static_cast<u32>(got));
                        w32(ctx, item + 8, session);
                        w8(ctx, s + fs::kFill, static_cast<u8>((static_cast<s8>(r8(ctx, s + fs::kFill)) + 1) % 2));
                        atomic_store_u8(gb(ctx), s + fs::kReady, static_cast<u8>(atomic_load_u8(gb(ctx), s + fs::kReady) + 1));
                        stream_out(ctx, s);
                        break;
                    }
                }
                // The length read, or the file's end.
                timeout = kSecond;
                input_eos(ctx, s);
                w32(ctx, s + fs::kState, kOpened);
                break;
            }
            }
        }
    }
    if (r < 0 || static_cast<u32>(r) == lv2::CELL_ETIMEDOUT) {
        // A failed lock, wait or signal: the reader ends, the session with it.
        w32(ctx, s + fs::kState, kEnded);
        session_error(ctx, s, r, 1);
    }
    if (fd()) file_call(ctx, s, kClose, {fdx()});
    lv2_lib(ctx, "sys_ppu_thread_exit", {sx(r)});
}

// ------------------------------------------------------------------------------------------------- the callbacks

// Makeup(source, protocol): cellFs loaded, the lwmutex and lwcond made, the reader started (it signals once it runs).
s32 _pspocket_cellSail_srcMakeup(PPUContext& ctx, u32 s, u32 /*protocol*/)
{
    s32 r = load_module(ctx, 0xE);
    if (r < 0) return r;
    r = make_lwmutex(ctx, s + fs::kMutex, "_saifs0");
    if (r >= 0) {
        StackVar cattr(ctx, gb(ctx), 16); // sys_lwcond_attribute_t: the name
        std::memcpy(gb(ctx) + cattr.addr(), "_saifs0", 8);
        r = static_cast<s32>(lv2_lib(ctx, "sys_lwcond_create", {s + fs::kCond, s + fs::kMutex, cattr.addr()}));
        if (r >= 0) {
            r = lock(ctx, s);
            if (r >= 0) {
                const u32 p = r32(ctx, s + fs::kSource);
                const u32 prio = r32(ctx, p + 0x3A8), stack = r32(ctx, p + 0x3AC);
                StackVar name(ctx, gb(ctx), 32);
                std::memcpy(gb(ctx) + name.addr(), "_libsail-source_fs", 19);
                r = static_cast<s32>(lv2_lib(ctx, "sys_ppu_thread_create", {s + fs::kThread,
                    internal("_pspocket_cellSail_srcThread"), s, sx(static_cast<s32>(prio ? prio : 1200)),
                    stack ? stack : 0x1000, 1, name.addr()}));
                if (r >= 0) {
                    wait(ctx, s, 0);
                    unlock(ctx, s);
                    return lv2::CELL_OK;
                }
                unlock(ctx, s);
            }
            lv2_lib(ctx, "sys_lwcond_destroy", {s + fs::kCond});
        }
        destroy_lwmutex(ctx, s + fs::kMutex);
    }
    unload_module(ctx, 0xE);
    return r;
}

// Cleanup(source): the open file's calls cancelled, the reader told to end and joined, the buffer freed, the lwmutex
// and lwcond destroyed, cellFs unloaded. The descriptor of a file still open stays in the source (the reader closed
// it): libsail's.
s32 _pspocket_cellSail_srcCleanup(PPUContext& ctx, u32 s)
{
    Frame f(ctx, 176);
    lock(ctx, s);
    if (const u32 fd = atomic_load_be32(gb(ctx), s + fs::kFd)) file_call(ctx, s, kCancel, {sx(static_cast<s32>(fd))});
    const s32 r = post_command(ctx, s, kCmdEnd);
    unlock(ctx, s);
    if (r >= 0) sc(ctx, 44, {r64(ctx, s + fs::kThread), f.at(112)}); // sys_ppu_thread_join
    free_buffer(ctx, s);
    lv2_lib(ctx, "sys_lwcond_destroy", {s + fs::kCond});
    destroy_lwmutex(ctx, s + fs::kMutex);
    unload_module(ctx, 0xE);
    w32(ctx, s + fs::kPath, 0);
    w32(ctx, s + fs::kState, kEnded);
    return lv2::CELL_OK;
}

// Open(source, stream type, media info, URI, profile): the reader takes the command, then the path is copied (marked
// changed when it is not the one the reader has) and the 2 MB buffer allocated (both items' halves, 1 MB a read); the
// reader answers. Errors before that are answered here. Without the buffer, the path is cleared: the reader then does
// nothing (and the second item points at 1 MB: libsail's).
void _pspocket_cellSail_srcOpen(PPUContext& ctx, u32 s, s32 /*type*/, u32 /*media*/, u32 uri, u32 /*profile*/)
{
    if (!usable(ctx, uri + 12, 1, "URI")) {
        completed(ctx, s, error(CELL_SAIL_ERROR_INVALID_ARG));
        return;
    }
    s32 r = lock(ctx, s);
    if (r >= 0) {
        r = post_command(ctx, s, kCmdOpen);
        if (r >= 0) r = wait_taken(ctx, s);
        if (r >= 0) {
            const u32 path = r32(ctx, s + fs::kPath);
            if (lv2_lib(ctx, "_sys_strncmp", {path, uri + 12, 287}) != 0) {
                lv2_lib(ctx, "_sys_strncpy", {path, uri + 12, 287});
                w8(ctx, path + 286, 0);
                w8(ctx, s + fs::kPathChanged, 1);
            }
            r = 0;
            if (!atomic_load_be32(gb(ctx), s + fs::kItems)) {
                const u32 p = r32(ctx, s + fs::kSource);
                const u32 buf = static_cast<u32>(call_guest(ctx, r32(ctx, p + pl::kAlloc),
                    {r32(ctx, p + pl::kAllocArg), 0x80, fs::kBufferSize}));
                if (!buf) {
                    w8(ctx, path, 0);
                    w8(ctx, s + fs::kPathChanged, 1);
                    r = error(CELL_SAIL_ERROR_MEMORY);
                }
                atomic_store_be32(gb(ctx), s + fs::kItems + fs::kItemSize, buf + fs::kChunkSize);
                w32(ctx, s + fs::kChunk, fs::kChunkSize);
                atomic_store_be32(gb(ctx), s + fs::kItems, buf);
            }
            signal(ctx, s);
        }
        unlock(ctx, s);
    }
    if (r < 0) completed(ctx, s, r);
}

// Close(source): the open file's calls cancelled, the reader told to close it, the buffer freed at once.
void _pspocket_cellSail_srcClose(PPUContext& ctx, u32 s)
{
    s32 r = lock(ctx, s);
    if (r >= 0) {
        if (const u32 fd = atomic_load_be32(gb(ctx), s + fs::kFd)) file_call(ctx, s, kCancel, {sx(static_cast<s32>(fd))});
        r = post_command(ctx, s, kCmdClose);
        if (r > 0) r = 0;
        unlock(ctx, s);
        free_buffer(ctx, s);
    }
    if (r < 0) completed(ctx, s, r);
}

// Start(source, CellSailSourceStartCommand, session): only from a byte position (start flags 1); the position and
// length kept for the reader once it took the last start.
void _pspocket_cellSail_srcStart(PPUContext& ctx, u32 s, u32 command, u32 session)
{
    if (!usable(ctx, command, 24, "start command")) {
        completed(ctx, s, error(CELL_SAIL_ERROR_INVALID_ARG));
        return;
    }
    s32 r = error(CELL_SAIL_ERROR_INVALID_ARG);
    if (r64(ctx, command) == 1) {
        r = lock(ctx, s);
        if (r >= 0) {
            for (;;) {
                if (r32(ctx, s + fs::kSession) == 0) {
                    w64(ctx, s + fs::kStart, r64(ctx, command + 8));
                    w32(ctx, s + fs::kSession, session);
                    w64(ctx, s + fs::kLength, r64(ctx, command + 16));
                    r = post_command(ctx, s, kCmdStart);
                    if (r >= 0) r = 0;
                    break;
                }
                r = wait(ctx, s, 0);
                if (r < 0) break;
            }
            unlock(ctx, s);
            if (r >= 0) return;
        }
    }
    completed(ctx, s, r);
}

// Stop(source): the open file's calls cancelled, the reader told to stop. libsail goes on when its lock fails.
void _pspocket_cellSail_srcStop(PPUContext& ctx, u32 s)
{
    s32 r = lock(ctx, s);
    if (const u32 fd = atomic_load_be32(gb(ctx), s + fs::kFd)) file_call(ctx, s, kCancel, {sx(static_cast<s32>(fd))});
    const s32 posted = post_command(ctx, s, kCmdStop);
    if (posted < 0) r = posted;
    else if (r > 0) r = 0;
    unlock(ctx, s);
    if (r < 0) completed(ctx, s, r);
}

// Cancel(source): nothing.
void _pspocket_cellSail_srcCancel(PPUContext&, u32) {}

// Checkout(source, CellSailSourceBufferItem**): the next filled item (the items taken in turn; Checkin, not Checkout,
// counts one less ready), CELL_SAIL_ERROR_FATAL when none is.
s32 _pspocket_cellSail_srcCheckout(PPUContext& ctx, u32 s, u32 item)
{
    s32 r = lock(ctx, s);
    if (r < 0) return r;
    if (static_cast<s8>(atomic_load_u8(gb(ctx), s + fs::kReady)) <= 0) {
        r = error(CELL_SAIL_ERROR_FATAL);
    } else if (!usable(ctx, item, 4, "item pointer")) {
        r = error(CELL_SAIL_ERROR_INVALID_ARG);
    } else {
        const s32 take = static_cast<s8>(atomic_load_u8(gb(ctx), s + fs::kTake));
        w32(ctx, item, s + fs::kItems + fs::kItemSize * static_cast<u32>(take));
        atomic_store_u8(gb(ctx), s + fs::kTake, static_cast<u8>((take + 1) % 2));
    }
    unlock(ctx, s);
    return r;
}

// Checkin(source, item): one item fewer ready (whichever it is), for the reader.
s32 _pspocket_cellSail_srcCheckin(PPUContext& ctx, u32 s, u32 /*item*/)
{
    s32 r = lock(ctx, s);
    if (r < 0) return r;
    r = post_command(ctx, s, kCmdCheckin);
    unlock(ctx, s);
    return r;
}

// Clear(source): the reader forgets its items.
s32 _pspocket_cellSail_srcClear(PPUContext& ctx, u32 s)
{
    s32 r = lock(ctx, s);
    if (r < 0) return r;
    r = post_command(ctx, s, kCmdClear);
    unlock(ctx, s);
    return r;
}

// Read(source, stream type, media info, URI, offset, buffer, size, u64* total size): the reader reads it (as ReadSync)
// and answers; the arguments go to it once it took the command.
s32 _pspocket_cellSail_srcRead(PPUContext& ctx, u32 s, s32 /*type*/, u32 media, u32 uri, u64 offset, u32 buf, u32 size,
    u32 total)
{
    s32 r = lock(ctx, s);
    if (r < 0) return r;
    const u32 args = r32(ctx, s + fs::kReadArgs);
    r = post_command(ctx, s, kCmdRead);
    if (r >= 0) r = wait_taken(ctx, s);
    if (r >= 0) {
        w32(ctx, args + 24, total);
        w32(ctx, args, uri);
        w32(ctx, args + 4, media);
        w64(ctx, args + 8, offset);
        w32(ctx, args + 16, buf);
        w32(ctx, args + 20, size);
        r = signal(ctx, s);
    }
    unlock(ctx, s);
    return r;
}

// ReadSync(source, stream type, media info, URI, offset, buffer, size, u64* total size): read at once on the caller's
// thread (open, size, seek, read, close).
s32 _pspocket_cellSail_srcReadSync(PPUContext& ctx, u32 s, s32 /*type*/, u32 /*media*/, u32 uri, u64 offset, u32 buf,
    u32 size, u32 total)
{
    return read_once(ctx, s, uri, offset, buf, size, total, false);
}

// GetCapabilities(source, stream type, media info, URI, u64* capabilities): 0x71 whatever the stream.
s32 _pspocket_cellSail_srcGetCapabilities(PPUContext& ctx, u32, s32, u32, u32, u32 caps)
{
    if (!usable(ctx, caps, 8, "capabilities' u64")) return error(CELL_SAIL_ERROR_INVALID_ARG);
    w64(ctx, caps, 0x71);
    return lv2::CELL_OK;
}

// InquireCapability(source, stream type, media info, URI, CellSailSourceStartCommand*): libsail refuses exactly the
// start flags Start takes (1) and takes every other.
s32 _pspocket_cellSail_srcInquireCapability(PPUContext& ctx, u32, s32, u32, u32, u32 command)
{
    if (!usable(ctx, command, 8, "start command")) return error(CELL_SAIL_ERROR_INVALID_ARG);
    return r64(ctx, command) == 1 ? error(CELL_SAIL_ERROR_INVALID_ARG) : lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- the file calls

// libsail's own file calls: cellFs's, the registers passed on as they are.
s32 fs_call(PPUContext& ctx, std::string_view name)
{
    return static_cast<s32>(module_call(ctx, "sys_fs", name,
        {ctx.gpr[3], ctx.gpr[4], ctx.gpr[5], ctx.gpr[6], ctx.gpr[7], ctx.gpr[8], ctx.gpr[9], ctx.gpr[10]}));
}
s32 _pspocket_cellSail_fsOpen(PPUContext& ctx) { return fs_call(ctx, "cellFsOpen"); }
s32 _pspocket_cellSail_fsClose(PPUContext& ctx) { return fs_call(ctx, "cellFsClose"); }
s32 _pspocket_cellSail_fsFstat(PPUContext& ctx) { return fs_call(ctx, "cellFsFstat"); }
s32 _pspocket_cellSail_fsRead(PPUContext& ctx) { return fs_call(ctx, "cellFsRead"); }
s32 _pspocket_cellSail_fsLseek(PPUContext& ctx) { return fs_call(ctx, "cellFsLseek"); }
s32 _pspocket_cellSail_fsCancel(PPUContext&) { return lv2::CELL_OK; }

// The file calls' 14 words: -1, open, none, close, fstat, read, lseek, cancel, then none.
void own_file_calls(PPUContext& ctx, u32 s)
{
    const u32 calls[14] = {0xFFFFFFFF, internal("_pspocket_cellSail_fsOpen"), 0, internal("_pspocket_cellSail_fsClose"),
        internal("_pspocket_cellSail_fsFstat"), internal("_pspocket_cellSail_fsRead"),
        internal("_pspocket_cellSail_fsLseek"), internal("_pspocket_cellSail_fsCancel"), 0, 0, 0, 0, 0, 0};
    for (u32 i = 0; i < 14; i++) w32(ctx, s + fs::kCalls + 4 * i, calls[i]);
}

} // namespace

s32 builtin_source_init(PPUContext& ctx, u32 s)
{
    u8* b = gb(ctx);
    std::memset(b + s, 0, fsrc::kSize);
    own_file_calls(ctx, s);
    static constexpr std::string_view kFuncs[14] = {"_pspocket_cellSail_srcMakeup", "_pspocket_cellSail_srcCleanup",
        "_pspocket_cellSail_srcOpen", "_pspocket_cellSail_srcClose", "_pspocket_cellSail_srcStart",
        "_pspocket_cellSail_srcStop", "_pspocket_cellSail_srcCancel", "_pspocket_cellSail_srcCheckout",
        "_pspocket_cellSail_srcCheckin", "_pspocket_cellSail_srcClear", "_pspocket_cellSail_srcRead",
        "_pspocket_cellSail_srcReadSync", "_pspocket_cellSail_srcGetCapabilities",
        "_pspocket_cellSail_srcInquireCapability"};
    // CellSailSourceFuncs, on the stack where libsail's is its data (cellSailSourceInitialize copies it).
    StackVar funcs(ctx, b, 0x38);
    for (u32 i = 0; i < 14; i++) w32(ctx, funcs.addr() + 4 * i, internal(kFuncs[i]));
    return static_cast<s32>(module_call(ctx, "cellSail", "cellSailSourceInitialize", {s + fsrc::kSource, funcs.addr(), s}));
}

void builtin_source_fini(PPUContext& ctx, u32 s)
{
    module_call(ctx, "cellSail", "cellSailSourceFinalize", {s + fsrc::kSource});
}

s32 builtin_source_set_file_calls(PPUContext& ctx, u32 s, u32 calls)
{
    if (!calls) {
        own_file_calls(ctx, s);
        return lv2::CELL_OK;
    }
    if (r32(ctx, calls) != 0 || !r32(ctx, calls + 4 * kOpen) || !r32(ctx, calls + 4 * kClose) ||
        !r32(ctx, calls + 4 * kFstat) || !r32(ctx, calls + 4 * kRead) || !r32(ctx, calls + 4 * kLseek) ||
        !r32(ctx, calls + 4 * kCancel))
        return error(CELL_SAIL_ERROR_INVALID_ARG);
    std::memmove(gb(ctx) + s + fs::kCalls, gb(ctx) + calls, 0x28);
    return lv2::CELL_OK;
}

} // namespace sail

#define SAIL_INTERNAL(n)                                                                                               \
    ::pspocket::hle::Function{{}, "_pspocket_cellSail_" #n, recomp::nid_of("_pspocket_cellSail_" #n),                  \
        &detail::thunk<&sail::_pspocket_cellSail_##n>, true}

void register_cellSailFileSource(Registry& r)
{
    r.add("cellSail", {
        SAIL_INTERNAL(fsOpen),
        SAIL_INTERNAL(fsClose),
        SAIL_INTERNAL(fsFstat),
        SAIL_INTERNAL(fsRead),
        SAIL_INTERNAL(fsLseek),
        SAIL_INTERNAL(fsCancel),
        SAIL_INTERNAL(srcThread),
        SAIL_INTERNAL(srcMakeup),
        SAIL_INTERNAL(srcCleanup),
        SAIL_INTERNAL(srcOpen),
        SAIL_INTERNAL(srcClose),
        SAIL_INTERNAL(srcStart),
        SAIL_INTERNAL(srcStop),
        SAIL_INTERNAL(srcCancel),
        SAIL_INTERNAL(srcCheckout),
        SAIL_INTERNAL(srcCheckin),
        SAIL_INTERNAL(srcClear),
        SAIL_INTERNAL(srcRead),
        SAIL_INTERNAL(srcReadSync),
        SAIL_INTERNAL(srcGetCapabilities),
        SAIL_INTERNAL(srcInquireCapability),
    });
}
#undef SAIL_INTERNAL

} // namespace pspocket::hle
