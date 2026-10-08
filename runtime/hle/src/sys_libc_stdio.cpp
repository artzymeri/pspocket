// sys_libc part 4 (D-514): stdio.h and the POSIX file functions, over lv2's file system and its TTY.
//
// A FILE is 128 bytes of guest memory, cleared but for its first word, which holds its own address: the getc and putc
// macros of Dinkumware's headers compare buffer pointers in it and so always call the functions, and code that takes
// the standard streams' variables (_Stdin, _Stdout, _Stderr) for FILE pointers rather than FILEs still finds the
// stream. What the library keeps of a stream lives on the host (its descriptor, mode, buffers, indicators, pushed-back
// bytes); data moves through a staging buffer of the stream's in guest memory, the C library's 4 KB, since lv2 reads
// and writes guest memory.
// - The console: descriptors 0, 1 and 2 (lv2's own start at 3) are stdin, stdout and stderr. Output goes to the TTY
//   (sys_tty_write channel 0, as liblv2's printf); stdout is line buffered, stderr unbuffered. lv2's TTY gives no input
//   (sys_tty_read fails), so stdin is at its end at once.
// - Files: fopen's modes as C99 (r, w, a, each with + and b) on lv2's flags (the C library's fcntl.h has lv2's
//   O_* values); fully buffered; text and binary alike (no translation). C99's rules where they leave the library a
//   choice: the end-of-file indicator sticks (fgetc returns EOF while it is set), ungetc may push back any number of
//   bytes, a write after reading (or a read after writing) needs no seek in between: the library repositions itself.
// - fpos_t is Dinkumware's: the 64-bit offset, then an 8-byte conversion state (cleared).
// - tmpnam's names are "/dev_hdd1/tNNNN" (15 characters, so a buffer of Dinkumware's L_tmpnam takes them); tmpfile
//   creates one and removes it at fclose.
// - errno: lv2's error for a failed lv2 call, EBADF for a stream opened the other way, EINVAL for a bad mode.

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "libc_format.hpp"
#include "libc_guest.hpp"
#include "libc_malloc.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

using namespace libc;

constexpr s32 kEOF = -1;
constexpr u32 kWEOF = 0xFFFFFFFFu;
constexpr u32 kFileBytes = 128;
constexpr s32 kIOFBF = 0, kIOLBF = 1, kIONBF = 2; // Dinkumware's setvbuf modes
constexpr u32 kBuffer = 4096;                     // a stream's buffer
constexpr s32 kSeekSet = 0, kSeekCur = 1, kSeekEnd = 2;

// lv2's open flags (CELL_FS_O_*), which the C library's fcntl.h has.
constexpr s32 kORdOnly = 0, kOWrOnly = 1, kORdWr = 2, kOCreat = 0x40, kOExcl = 0x80, kOTrunc = 0x200,
              kOAppend = 0x400;
// Dinkumware's _Mode bits (xstdio.h), for _Fopen's mode argument.
constexpr u32 kMOpenR = 0x1, kMOpenW = 0x2, kMOpenA = 0x4, kMTrunc = 0x8, kMCreat = 0x10;

constexpr s32 kStdinFd = 0, kStdoutFd = 1, kStderrFd = 2;

u8* gb(PPUContext& ctx) { return guest_base(ctx); }

void set_lv2_errno(PPUContext& ctx, lv2::Result r) { set_errno(ctx, static_cast<u32>(r)); }

// ------------------------------------------------------------------------------------------------- lv2 calls

struct Lv2Fs {
    PPUContext& ctx;
    lv2::PpuThread& t;
    explicit Lv2Fs(PPUContext& c) : ctx(c), t(thread_of(c)) {}

    lv2::Result open(u32 path, s32 flags, s32 mode, s32* fd)
    {
        StackVar out(ctx, t.kernel.base(), 8);
        const lv2::Result r = lv2::sys_fs_open(t, path, flags, out.addr(), mode, 0, 0);
        if (r == lv2::CELL_OK) *fd = static_cast<s32>(ps_read32(t.kernel.base(), out.addr()));
        return r;
    }
    lv2::Result read(s32 fd, u32 buf, u64 n, u64* got)
    {
        StackVar out(ctx, t.kernel.base(), 8);
        const lv2::Result r = lv2::sys_fs_read(t, static_cast<u32>(fd), buf, n, out.addr());
        *got = r == lv2::CELL_OK ? ps_read64(t.kernel.base(), out.addr()) : 0;
        return r;
    }
    lv2::Result write(s32 fd, u32 buf, u64 n, u64* put)
    {
        StackVar out(ctx, t.kernel.base(), 8);
        const lv2::Result r = lv2::sys_fs_write(t, static_cast<u32>(fd), buf, n, out.addr());
        *put = r == lv2::CELL_OK ? ps_read64(t.kernel.base(), out.addr()) : 0;
        return r;
    }
    lv2::Result seek(s32 fd, s64 offset, s32 whence, s64* pos)
    {
        StackVar out(ctx, t.kernel.base(), 8);
        const lv2::Result r = lv2::sys_fs_lseek(t, static_cast<u32>(fd), offset, whence, out.addr());
        if (r == lv2::CELL_OK) *pos = static_cast<s64>(ps_read64(t.kernel.base(), out.addr()));
        return r;
    }
    lv2::Result close(s32 fd) { return lv2::sys_fs_close(t, static_cast<u32>(fd)); }
};

// ------------------------------------------------------------------------------------------------- streams

struct Stream {
    std::recursive_mutex lock; // every call on the stream, and _Lockfilelock
    u32 file = 0;
    s32 fd = -1;
    bool console = false; // fd 0..2
    bool readable = false, writable = false, append = false;
    bool eof = false, error = false;
    s32 orientation = 0;  // fwide: < 0 bytes, > 0 wide
    s32 mode = kIOFBF;
    bool owned = false;   // the FILE came from the library's malloc (freed at fclose)
    std::string out;      // written, not yet given to the descriptor
    std::string in;       // read ahead
    usize in_pos = 0;
    std::string back;     // pushed back (ungetc): the next byte read is the last
    u32 stage = 0;        // the guest buffer data moves through (kBuffer bytes, from malloc)
    std::string tmp_path; // tmpfile's file, removed at fclose
};

struct Stdio {
    std::mutex mutex; // the tables
    std::map<u32, std::shared_ptr<Stream>> streams; // by FILE
    bool std_made = false;
    u32 tmp_next = 0;
    u32 tmpnam_buf = 0; // tmpnam(NULL)'s name (16 bytes from malloc)
    std::map<s32, s32> fd_flags; // open()'s flags, for fcntl
};

Stdio& stdio() { return HleProcess::current()->state<Stdio>(); }

u32 std_file(const char* name) { return variable(recomp::nid_of(name)); }

void init_file(u8* g, u32 file)
{
    std::memset(g + file, 0, kFileBytes);
    ps_write32(g, file, file);
}

// The standard streams, made the first time stdio is used.
void ensure_std(PPUContext& ctx, Stdio& s)
{
    if (s.std_made) return;
    s.std_made = true;
    const struct {
        const char* name;
        s32 fd;
        bool readable;
        s32 mode;
    } kStd[] = {{"_Stdin", kStdinFd, true, kIOLBF}, {"_Stdout", kStdoutFd, false, kIOLBF},
        {"_Stderr", kStderrFd, false, kIONBF}};
    for (const auto& d : kStd) {
        const u32 file = std_file(d.name);
        if (!file) continue;
        auto st = std::make_shared<Stream>();
        st->file = file;
        st->fd = d.fd;
        st->console = true;
        st->readable = d.readable;
        st->writable = !d.readable;
        st->mode = d.mode;
        s.streams[file] = std::move(st);
    }
    (void)ctx;
}

std::shared_ptr<Stream> find_stream(PPUContext& ctx, u32 file)
{
    Stdio& s = stdio();
    std::lock_guard g(s.mutex);
    ensure_std(ctx, s);
    const auto it = s.streams.find(file);
    return it == s.streams.end() ? nullptr : it->second;
}

std::shared_ptr<Stream> stream_of(PPUContext& ctx, const char* fn, u32 file)
{
    auto st = find_stream(ctx, file);
    if (!st) refuse(ctx, std::format("{}({:#x}): not an open FILE", fn, file));
    return st;
}

u32 stage_of(PPUContext& ctx, Stream& st)
{
    if (!st.stage) st.stage = lib_malloc(ctx, kBuffer);
    return st.stage;
}

// Bytes to the stream's descriptor: the TTY for the console, lv2's file otherwise. False on an error (errno set).
bool raw_write(PPUContext& ctx, Stream& st, std::string_view data)
{
    if (data.empty()) return true;
    if (st.console) {
        if (st.fd == kStdinFd) {
            set_errno(ctx, kEBADF);
            return false;
        }
        tty_out(ctx, data);
        return true;
    }
    const u32 stage = stage_of(ctx, st);
    if (!stage) return false;
    Lv2Fs fs(ctx);
    while (!data.empty()) {
        const u32 n = static_cast<u32>(std::min<usize>(data.size(), kBuffer));
        std::memcpy(gb(ctx) + stage, data.data(), n);
        u64 put = 0;
        if (const lv2::Result r = fs.write(st.fd, stage, n, &put); r != lv2::CELL_OK) {
            set_lv2_errno(ctx, r);
            return false;
        }
        if (!put) {
            set_errno(ctx, kEIO);
            return false;
        }
        data.remove_prefix(static_cast<usize>(std::min<u64>(put, n)));
    }
    return true;
}

bool flush_out(PPUContext& ctx, Stream& st)
{
    if (st.out.empty()) return true;
    const bool ok = raw_write(ctx, st, st.out);
    st.out.clear();
    if (!ok) st.error = true;
    return ok;
}

// The bytes read ahead and pushed back that the program has not had: where the program is, before lv2's position.
s64 unread(const Stream& st) { return static_cast<s64>(st.in.size() - st.in_pos + st.back.size()); }

// Drops what was read ahead and pushed back, putting lv2's position where the program is.
bool drop_input(PPUContext& ctx, Stream& st)
{
    const s64 behind = unread(st);
    st.in.clear();
    st.in_pos = 0;
    st.back.clear();
    if (!behind || st.console) return true;
    s64 pos = 0;
    if (const lv2::Result r = Lv2Fs(ctx).seek(st.fd, -behind, kSeekCur, &pos); r != lv2::CELL_OK) {
        set_lv2_errno(ctx, r);
        st.error = true;
        return false;
    }
    return true;
}

bool to_write(PPUContext& ctx, Stream& st)
{
    if (!st.writable) {
        st.error = true;
        set_errno(ctx, kEBADF);
        return false;
    }
    if (st.in.size() != st.in_pos || !st.back.empty()) return drop_input(ctx, st);
    st.in.clear();
    st.in_pos = 0;
    return true;
}

bool put_bytes(PPUContext& ctx, Stream& st, std::string_view data)
{
    if (!to_write(ctx, st)) return false;
    st.out.append(data);
    if (st.mode == kIONBF || st.out.size() >= kBuffer ||
        (st.mode == kIOLBF && data.find('\n') != std::string_view::npos))
        return flush_out(ctx, st);
    return true;
}

// Reading from the console flushes stdout first (C99 7.19.3: line-buffered output is flushed when input is asked for).
void flush_stdout(PPUContext& ctx)
{
    if (const auto out = find_stream(ctx, std_file("_Stdout"))) {
        std::lock_guard l(out->lock);
        flush_out(ctx, *out);
    }
}

// Fills the read-ahead buffer: false at the end (eof set) or on an error (error set).
bool fill(PPUContext& ctx, Stream& st)
{
    if (!st.readable) {
        st.error = true;
        set_errno(ctx, kEBADF);
        return false;
    }
    if (st.eof) return false;
    if (!st.out.empty() && !flush_out(ctx, st)) return false;
    st.in.clear();
    st.in_pos = 0;
    if (st.console) {
        flush_stdout(ctx);
        st.eof = true; // lv2's TTY has no input
        return false;
    }
    const u32 stage = stage_of(ctx, st);
    if (!stage) {
        st.error = true;
        return false;
    }
    u64 got = 0;
    if (const lv2::Result r = Lv2Fs(ctx).read(st.fd, stage, kBuffer, &got); r != lv2::CELL_OK) {
        set_lv2_errno(ctx, r);
        st.error = true;
        return false;
    }
    if (!got) {
        st.eof = true;
        return false;
    }
    st.in.assign(reinterpret_cast<const char*>(gb(ctx) + stage), static_cast<usize>(std::min<u64>(got, kBuffer)));
    return true;
}

// The next byte, or -1 at the end or on an error.
s32 get_byte(PPUContext& ctx, Stream& st)
{
    if (!st.back.empty()) {
        const u8 c = static_cast<u8>(st.back.back());
        st.back.pop_back();
        return c;
    }
    if (st.in_pos < st.in.size()) return static_cast<u8>(st.in[st.in_pos++]);
    if (!fill(ctx, st)) return -1;
    return static_cast<u8>(st.in[st.in_pos++]);
}

void unget_byte(Stream& st, u8 c)
{
    st.back.push_back(static_cast<char>(c));
    st.eof = false;
}

void orient(Stream& st, s32 o)
{
    if (!st.orientation) st.orientation = o;
}

// The program's position in a file stream (output flushed first), or -1 (errno set).
s64 tell(PPUContext& ctx, Stream& st)
{
    if (st.console) {
        set_errno(ctx, kESPIPE);
        return -1;
    }
    if (!flush_out(ctx, st)) return -1;
    s64 pos = 0;
    if (const lv2::Result r = Lv2Fs(ctx).seek(st.fd, 0, kSeekCur, &pos); r != lv2::CELL_OK) {
        set_lv2_errno(ctx, r);
        return -1;
    }
    return std::max<s64>(pos - unread(st), 0);
}

// fseek's work: output flushed, input dropped, the end-of-file indicator cleared. 0, or -1 (errno set).
s32 seek(PPUContext& ctx, Stream& st, s64 offset, s32 whence)
{
    if (whence != kSeekSet && whence != kSeekCur && whence != kSeekEnd) {
        set_errno(ctx, kEINVAL);
        return -1;
    }
    if (st.console) {
        set_errno(ctx, kESPIPE);
        return -1;
    }
    if (!flush_out(ctx, st)) return -1;
    if (whence == kSeekCur) offset -= unread(st);
    st.in.clear();
    st.in_pos = 0;
    st.back.clear();
    s64 pos = 0;
    if (const lv2::Result r = Lv2Fs(ctx).seek(st.fd, offset, whence, &pos); r != lv2::CELL_OK) {
        set_lv2_errno(ctx, r);
        return -1;
    }
    st.eof = false;
    return 0;
}

// ------------------------------------------------------------------------------------------------- opening

struct OpenMode {
    s32 flags = 0;
    bool read = false, write = false, append = false;
};

// fopen's mode (C99 7.19.5.3): r, w or a, then any of + and b (and C11's x after w).
std::optional<OpenMode> parse_mode(std::string_view m)
{
    if (m.empty()) return std::nullopt;
    OpenMode o;
    bool plus = false, excl = false;
    for (usize i = 1; i < m.size(); i++) {
        if (m[i] == '+') plus = true;
        else if (m[i] == 'x') excl = true;
        else if (m[i] != 'b' && m[i] != 't') break;
    }
    switch (m[0]) {
    case 'r':
        o.read = true;
        o.write = plus;
        o.flags = plus ? kORdWr : kORdOnly;
        break;
    case 'w':
        o.write = true;
        o.read = plus;
        o.flags = (plus ? kORdWr : kOWrOnly) | kOCreat | kOTrunc | (excl ? kOExcl : 0);
        break;
    case 'a':
        o.write = true;
        o.read = plus;
        o.append = true;
        o.flags = (plus ? kORdWr : kOWrOnly) | kOCreat | kOAppend;
        break;
    default: return std::nullopt;
    }
    return o;
}

std::optional<OpenMode> mode_at(PPUContext& ctx, const char* fn, u32 mode)
{
    const auto m = read_string(thread_of(ctx), mode, 64);
    if (!m) {
        refuse(ctx, std::format("{}: the mode at {:#x} runs out of guest memory", fn, mode));
        return std::nullopt;
    }
    const auto o = parse_mode(*m);
    if (!o) set_errno(ctx, kEINVAL);
    return o;
}

// A stream for `fd` in `file` (a new FILE from malloc when 0).
u32 make_stream(PPUContext& ctx, u32 file, s32 fd, const OpenMode& o)
{
    bool owned = false;
    if (!file) {
        file = lib_malloc(ctx, kFileBytes);
        if (!file) return 0;
        owned = true;
    }
    init_file(gb(ctx), file);
    auto st = std::make_shared<Stream>();
    st->file = file;
    st->fd = fd;
    st->console = fd >= 0 && fd <= kStderrFd;
    st->readable = o.read;
    st->writable = o.write;
    st->append = o.append;
    st->mode = st->console ? (fd == kStderrFd ? kIONBF : kIOLBF) : kIOFBF;
    st->owned = owned;
    Stdio& s = stdio();
    std::lock_guard g(s.mutex);
    ensure_std(ctx, s);
    s.streams[file] = std::move(st);
    return file;
}

// Closes a stream's descriptor and forgets it (its FILE freed when the library made it). 0, or EOF.
s32 close_stream(PPUContext& ctx, const std::shared_ptr<Stream>& st)
{
    s32 rc = 0;
    {
        std::lock_guard l(st->lock);
        if (!flush_out(ctx, *st)) rc = kEOF;
        if (!st->console && st->fd >= 0) {
            if (const lv2::Result r = Lv2Fs(ctx).close(st->fd); r != lv2::CELL_OK) {
                set_lv2_errno(ctx, r);
                rc = kEOF;
            }
        }
        if (!st->tmp_path.empty()) {
            StackVar path(ctx, gb(ctx), static_cast<u32>(st->tmp_path.size() + 1));
            write_string(thread_of(ctx), path.addr(), st->tmp_path);
            lv2::sys_fs_unlink(thread_of(ctx), path.addr());
        }
        if (st->stage) lib_free(ctx, st->stage, "fclose");
        st->stage = 0;
        st->fd = -1;
    }
    Stdio& s = stdio();
    {
        std::lock_guard g(s.mutex);
        s.streams.erase(st->file);
    }
    if (st->owned) lib_free(ctx, st->file, "fclose");
    return rc;
}

u32 open_file(PPUContext& ctx, const char* fn, u32 path, u32 mode, u32 into)
{
    const auto o = mode_at(ctx, fn, mode);
    if (!o) return 0;
    if (!read_string(thread_of(ctx), path)) {
        refuse(ctx, std::format("{}: the path at {:#x} runs out of guest memory", fn, path));
        return 0;
    }
    s32 fd = -1;
    if (const lv2::Result r = Lv2Fs(ctx).open(path, o->flags, 0666, &fd); r != lv2::CELL_OK) {
        set_lv2_errno(ctx, r);
        return 0;
    }
    const u32 file = make_stream(ctx, into, fd, *o);
    if (!file) Lv2Fs(ctx).close(fd);
    return file;
}

u32 fopen_(PPUContext& ctx, u32 path, u32 mode) { return open_file(ctx, "fopen", path, mode, 0); }

// freopen: the stream's file closed, then `path` opened into the same FILE. A null path keeps the file when the new
// mode asks for no access it lacks (the implementation-defined changes of mode C99 allows), else fails.
u32 freopen_(PPUContext& ctx, u32 path, u32 mode, u32 file)
{
    const auto st = stream_of(ctx, "freopen", file);
    if (!st) return 0;
    if (!path) {
        const auto o = mode_at(ctx, "freopen", mode);
        std::lock_guard l(st->lock);
        if (!o || (o->read && !st->readable) || (o->write && !st->writable)) {
            set_errno(ctx, kEBADF);
            return 0;
        }
        flush_out(ctx, *st);
        st->eof = st->error = false;
        st->orientation = 0;
        return file;
    }
    const bool owned = st->owned;
    st->owned = false; // keep the FILE
    close_stream(ctx, st);
    const u32 r = open_file(ctx, "freopen", path, mode, file);
    if (r) {
        if (const auto again = find_stream(ctx, file)) again->owned = owned;
    } else if (owned) {
        lib_free(ctx, file, "freopen");
    }
    return r;
}

// fdopen: a stream on a POSIX descriptor (0..2 the console's).
u32 fdopen_(PPUContext& ctx, s32 fd, u32 mode)
{
    const auto o = mode_at(ctx, "fdopen", mode);
    if (!o) return 0;
    if (fd < 0) {
        set_errno(ctx, kEBADF);
        return 0;
    }
    return make_stream(ctx, 0, fd, *o);
}

s32 fclose_(PPUContext& ctx, u32 file)
{
    const auto st = stream_of(ctx, "fclose", file);
    if (!st) return kEOF;
    return close_stream(ctx, st);
}

// tmpnam: "/dev_hdd1/tNNNN", the first such name no file has (TMP_MAX 10000 names); NULL when all are taken.
std::string temp_name(PPUContext& ctx)
{
    Stdio& s = stdio();
    lv2::PpuThread& t = thread_of(ctx);
    for (u32 tries = 0; tries < 10000; tries++) {
        u32 n;
        {
            std::lock_guard g(s.mutex);
            n = s.tmp_next++ % 10000;
        }
        const std::string name = std::format("/dev_hdd1/t{:04}", n);
        StackVar buf(ctx, t.kernel.base(), 0x60);
        write_string(t, buf.addr(), name);
        const lv2::Result r = lv2::sys_fs_stat(t, buf.addr(), buf.addr() + 0x20);
        if (static_cast<u32>(r) == lv2::CELL_ENOENT) return name;
        if (r != lv2::CELL_OK) break; // no /dev_hdd1 to make names in
    }
    return {};
}

u32 tmpnam_(PPUContext& ctx, u32 s)
{
    const std::string name = temp_name(ctx);
    if (name.empty()) return 0;
    u32 out = s;
    if (!out) {
        Stdio& st = stdio();
        std::lock_guard g(st.mutex);
        if (!st.tmpnam_buf) st.tmpnam_buf = lib_malloc(ctx, 16);
        out = st.tmpnam_buf;
        if (!out) return 0;
    }
    if (!range_ok(ctx, "tmpnam", out, name.size() + 1)) return 0;
    write_string(thread_of(ctx), out, name);
    return out;
}

u32 tmpfile_(PPUContext& ctx)
{
    const std::string name = temp_name(ctx);
    if (name.empty()) {
        set_errno(ctx, kEEXIST);
        return 0;
    }
    StackVar path(ctx, gb(ctx), 0x20);
    write_string(thread_of(ctx), path.addr(), name);
    s32 fd = -1;
    if (const lv2::Result r = Lv2Fs(ctx).open(path.addr(), kORdWr | kOCreat | kOTrunc | kOExcl, 0666, &fd);
        r != lv2::CELL_OK) {
        set_lv2_errno(ctx, r);
        return 0;
    }
    const u32 file = make_stream(ctx, 0, fd, OpenMode{kORdWr, true, true, false});
    if (!file) {
        Lv2Fs(ctx).close(fd);
        return 0;
    }
    if (const auto st = find_stream(ctx, file)) st->tmp_path = name;
    return file;
}

// ------------------------------------------------------------------------------------------------- byte I/O

s32 fgetc_(PPUContext& ctx, u32 file)
{
    const auto st = stream_of(ctx, "fgetc", file);
    if (!st) return kEOF;
    std::lock_guard l(st->lock);
    orient(*st, -1);
    return get_byte(ctx, *st);
}

s32 getchar_(PPUContext& ctx) { return fgetc_(ctx, std_file("_Stdin")); }

// fgets: up to n - 1 bytes, through a newline; NULL when n is 1 or less (Dinkumware's fgets.c) or nothing was read.
u32 fgets_(PPUContext& ctx, u32 s, s32 n, u32 file)
{
    if (n <= 1) return 0;
    const auto st = stream_of(ctx, "fgets", file);
    if (!st || !range_ok(ctx, "fgets", s, static_cast<u32>(n))) return 0;
    std::lock_guard l(st->lock);
    orient(*st, -1);
    s32 i = 0;
    while (i < n - 1) {
        const s32 c = get_byte(ctx, *st);
        if (c < 0) break;
        gb(ctx)[s + static_cast<u32>(i++)] = static_cast<u8>(c);
        if (c == '\n') break;
    }
    if (!i || st->error) return 0;
    gb(ctx)[s + static_cast<u32>(i)] = 0;
    return s;
}

// gets: a line from stdin without its newline (no bound, as C99 has it); NULL when nothing was read.
u32 gets_(PPUContext& ctx, u32 s)
{
    const auto st = stream_of(ctx, "gets", std_file("_Stdin"));
    if (!st) return 0;
    std::lock_guard l(st->lock);
    std::string line;
    bool any = false;
    for (;;) {
        const s32 c = get_byte(ctx, *st);
        if (c < 0) break;
        any = true;
        if (c == '\n') break;
        line += static_cast<char>(c);
    }
    if (!any || st->error || !range_ok(ctx, "gets", s, line.size() + 1)) return 0;
    write_string(thread_of(ctx), s, line);
    return s;
}

s32 fputc_(PPUContext& ctx, s32 c, u32 file)
{
    const auto st = stream_of(ctx, "fputc", file);
    if (!st) return kEOF;
    std::lock_guard l(st->lock);
    orient(*st, -1);
    const char b = static_cast<char>(c);
    return put_bytes(ctx, *st, std::string_view(&b, 1)) ? static_cast<u8>(c) : kEOF;
}

s32 putchar_(PPUContext& ctx, s32 c) { return fputc_(ctx, c, std_file("_Stdout")); }

s32 put_text(PPUContext& ctx, const char* fn, u32 file, std::string_view text)
{
    const auto st = stream_of(ctx, fn, file);
    if (!st) return kEOF;
    std::lock_guard l(st->lock);
    orient(*st, -1);
    return put_bytes(ctx, *st, text) ? 0 : kEOF;
}

s32 fputs_(PPUContext& ctx, u32 s, u32 file)
{
    const auto text = read_string(thread_of(ctx), s);
    if (!text) {
        refuse(ctx, std::format("fputs: the string at {:#x} runs out of guest memory", s));
        return kEOF;
    }
    return put_text(ctx, "fputs", file, *text);
}

s32 puts_(PPUContext& ctx, u32 s)
{
    auto text = read_string(thread_of(ctx), s);
    if (!text) {
        refuse(ctx, std::format("puts: the string at {:#x} runs out of guest memory", s));
        return kEOF;
    }
    *text += '\n';
    return put_text(ctx, "puts", std_file("_Stdout"), *text);
}

s32 ungetc_(PPUContext& ctx, s32 c, u32 file)
{
    if (c == kEOF) return kEOF;
    const auto st = stream_of(ctx, "ungetc", file);
    if (!st) return kEOF;
    std::lock_guard l(st->lock);
    if (!st->readable) return kEOF;
    unget_byte(*st, static_cast<u8>(c));
    return static_cast<u8>(c);
}

// fread: whole elements; large reads with nothing buffered go straight from lv2 into the caller's memory.
u32 fread_(PPUContext& ctx, u32 buf, u32 size, u32 n, u32 file)
{
    const u64 total = u64{size} * n;
    if (!total) return 0;
    const auto st = stream_of(ctx, "fread", file);
    if (!st || !range_ok(ctx, "fread", buf, total)) return 0;
    std::lock_guard l(st->lock);
    orient(*st, -1);
    u64 done = 0;
    while (done < total) {
        if (!st->back.empty() || st->in_pos < st->in.size()) {
            const s32 c = get_byte(ctx, *st);
            if (c < 0) break;
            gb(ctx)[buf + done++] = static_cast<u8>(c);
            continue;
        }
        if (total - done >= kBuffer && !st->console && st->readable && !st->eof) {
            if (!st->out.empty() && !flush_out(ctx, *st)) break;
            u64 got = 0;
            if (const lv2::Result r = Lv2Fs(ctx).read(st->fd, static_cast<u32>(buf + done), total - done, &got);
                r != lv2::CELL_OK) {
                set_lv2_errno(ctx, r);
                st->error = true;
                break;
            }
            if (!got) {
                st->eof = true;
                break;
            }
            done += got;
            continue;
        }
        if (!fill(ctx, *st)) break;
    }
    return static_cast<u32>(done / size);
}

u32 fwrite_(PPUContext& ctx, u32 buf, u32 size, u32 n, u32 file)
{
    const u64 total = u64{size} * n;
    if (!total) return 0;
    const auto st = stream_of(ctx, "fwrite", file);
    if (!st || !range_ok(ctx, "fwrite", buf, total)) return 0;
    std::lock_guard l(st->lock);
    orient(*st, -1);
    if (total >= kBuffer && !st->console && st->writable) {
        if (!to_write(ctx, *st) || !flush_out(ctx, *st)) return 0;
        u64 done = 0;
        while (done < total) {
            u64 put = 0;
            if (const lv2::Result r = Lv2Fs(ctx).write(st->fd, static_cast<u32>(buf + done), total - done, &put);
                r != lv2::CELL_OK || !put) {
                set_lv2_errno(ctx, r != lv2::CELL_OK ? r : static_cast<lv2::Result>(kEIO));
                st->error = true;
                break;
            }
            done += put;
        }
        return static_cast<u32>(done / size);
    }
    const std::string_view data(reinterpret_cast<const char*>(gb(ctx) + buf), static_cast<usize>(total));
    return put_bytes(ctx, *st, data) ? n : 0;
}

// ------------------------------------------------------------------------------------------------- wide I/O

// In the "C" locale a wide character is a byte (D-512): fgetwc reads one, fputwc writes one (EILSEQ past 255).
u32 fgetwc_(PPUContext& ctx, u32 file)
{
    const auto st = stream_of(ctx, "fgetwc", file);
    if (!st) return kWEOF;
    std::lock_guard l(st->lock);
    orient(*st, 1);
    const s32 c = get_byte(ctx, *st);
    return c < 0 ? kWEOF : static_cast<u32>(c);
}

u32 getwchar_(PPUContext& ctx) { return fgetwc_(ctx, std_file("_Stdin")); }

u32 fputwc_(PPUContext& ctx, u32 wc, u32 file)
{
    const auto st = stream_of(ctx, "fputwc", file);
    if (!st) return kWEOF;
    std::lock_guard l(st->lock);
    orient(*st, 1);
    const auto b = wide_to_byte(static_cast<char32_t>(wc));
    if (!b) {
        set_errno(ctx, kEILSEQ);
        st->error = true;
        return kWEOF;
    }
    return put_bytes(ctx, *st, std::string_view(&*b, 1)) ? wc : kWEOF;
}

u32 putwchar_(PPUContext& ctx, u32 wc) { return fputwc_(ctx, wc, std_file("_Stdout")); }

u32 fgetws_(PPUContext& ctx, u32 ws, s32 n, u32 file)
{
    if (n <= 1) return 0;
    const auto st = stream_of(ctx, "fgetws", file);
    if (!st || !range_ok(ctx, "fgetws", ws, u64(n) * 4)) return 0;
    std::lock_guard l(st->lock);
    orient(*st, 1);
    s32 i = 0;
    while (i < n - 1) {
        const s32 c = get_byte(ctx, *st);
        if (c < 0) break;
        ps_write32(gb(ctx), ws + 4 * static_cast<u32>(i++), static_cast<u32>(c));
        if (c == '\n') break;
    }
    if (!i || st->error) return 0;
    ps_write32(gb(ctx), ws + 4 * static_cast<u32>(i), 0);
    return ws;
}

s32 put_wide(PPUContext& ctx, const char* fn, u32 file, std::u32string_view text)
{
    const auto st = stream_of(ctx, fn, file);
    if (!st) return kEOF;
    std::lock_guard l(st->lock);
    orient(*st, 1);
    std::string bytes;
    for (const char32_t c : text) {
        const auto b = wide_to_byte(c);
        if (!b) {
            set_errno(ctx, kEILSEQ);
            st->error = true;
            return kEOF;
        }
        bytes += *b;
    }
    return put_bytes(ctx, *st, bytes) ? 0 : kEOF;
}

s32 fputws_(PPUContext& ctx, u32 ws, u32 file)
{
    const auto text = read_wstring(thread_of(ctx), ws, kMaxString / 4);
    if (!text) {
        refuse(ctx, std::format("fputws: the wide string at {:#x} runs out of guest memory", ws));
        return kEOF;
    }
    return put_wide(ctx, "fputws", file, *text);
}

u32 ungetwc_(PPUContext& ctx, u32 wc, u32 file)
{
    if (wc == kWEOF || wc > 0xFF) return kWEOF;
    const auto st = stream_of(ctx, "ungetwc", file);
    if (!st) return kWEOF;
    std::lock_guard l(st->lock);
    if (!st->readable) return kWEOF;
    unget_byte(*st, static_cast<u8>(wc));
    return wc;
}

s32 fwide_(PPUContext& ctx, u32 file, s32 mode)
{
    const auto st = stream_of(ctx, "fwide", file);
    if (!st) return 0;
    std::lock_guard l(st->lock);
    if (mode) orient(*st, mode > 0 ? 1 : -1);
    return st->orientation;
}

// ------------------------------------------------------------------------------------------------- position, state

s32 fseek_(PPUContext& ctx, u32 file, s32 offset, s32 whence)
{
    const auto st = stream_of(ctx, "fseek", file);
    if (!st) return -1;
    std::lock_guard l(st->lock);
    return seek(ctx, *st, offset, whence);
}

// ftell: long is 32 bits; a position past it is EOVERFLOW.
s32 ftell_(PPUContext& ctx, u32 file)
{
    const auto st = stream_of(ctx, "ftell", file);
    if (!st) return -1;
    std::lock_guard l(st->lock);
    const s64 pos = tell(ctx, *st);
    if (pos > 0x7FFFFFFF) {
        set_errno(ctx, kEOVERFLOW);
        return -1;
    }
    return static_cast<s32>(pos);
}

void rewind_(PPUContext& ctx, u32 file)
{
    const auto st = stream_of(ctx, "rewind", file);
    if (!st) return;
    std::lock_guard l(st->lock);
    seek(ctx, *st, 0, kSeekSet);
    st->error = false;
}

s32 fgetpos_(PPUContext& ctx, u32 file, u32 pos)
{
    const auto st = stream_of(ctx, "fgetpos", file);
    if (!st || !range_ok(ctx, "fgetpos", pos, 16)) return -1;
    std::lock_guard l(st->lock);
    const s64 p = tell(ctx, *st);
    if (p < 0) return -1;
    ps_write64(gb(ctx), pos, static_cast<u64>(p));
    ps_write64(gb(ctx), pos + 8, 0);
    return 0;
}

s32 fsetpos_(PPUContext& ctx, u32 file, u32 pos)
{
    const auto st = stream_of(ctx, "fsetpos", file);
    if (!st || !range_ok(ctx, "fsetpos", pos, 16)) return -1;
    std::lock_guard l(st->lock);
    return seek(ctx, *st, static_cast<s64>(ps_read64(gb(ctx), pos)), kSeekSet);
}

s32 feof_(PPUContext& ctx, u32 file)
{
    const auto st = stream_of(ctx, "feof", file);
    if (!st) return 0;
    std::lock_guard l(st->lock);
    return st->eof ? 1 : 0;
}

s32 ferror_(PPUContext& ctx, u32 file)
{
    const auto st = stream_of(ctx, "ferror", file);
    if (!st) return 0;
    std::lock_guard l(st->lock);
    return st->error ? 1 : 0;
}

void clearerr_(PPUContext& ctx, u32 file)
{
    const auto st = stream_of(ctx, "clearerr", file);
    if (!st) return;
    std::lock_guard l(st->lock);
    st->eof = st->error = false;
}

s32 fileno_(PPUContext& ctx, u32 file)
{
    const auto st = stream_of(ctx, "fileno", file);
    if (!st) {
        set_errno(ctx, kEBADF);
        return -1;
    }
    return st->fd;
}

s32 flush_stream(PPUContext& ctx, Stream& st)
{
    std::lock_guard l(st.lock);
    if (!st.writable) return 0;
    return flush_out(ctx, st) ? 0 : kEOF;
}

// fflush(NULL) flushes every output stream.
s32 fflush_(PPUContext& ctx, u32 file)
{
    if (file) {
        const auto st = stream_of(ctx, "fflush", file);
        return st ? flush_stream(ctx, *st) : kEOF;
    }
    std::vector<std::shared_ptr<Stream>> all;
    {
        Stdio& s = stdio();
        std::lock_guard g(s.mutex);
        ensure_std(ctx, s);
        for (const auto& [f, st] : s.streams) all.push_back(st);
    }
    s32 rc = 0;
    for (const auto& st : all)
        if (flush_stream(ctx, *st)) rc = kEOF;
    return rc;
}

// setvbuf: the mode (full, line, none); the caller's buffer is not used (the stream's is the library's). Nonzero for
// a mode that is none of them.
s32 setvbuf_(PPUContext& ctx, u32 file, u32, s32 mode, u32)
{
    if (mode != kIOFBF && mode != kIOLBF && mode != kIONBF) return -1;
    const auto st = stream_of(ctx, "setvbuf", file);
    if (!st) return -1;
    std::lock_guard l(st->lock);
    st->mode = mode;
    return flush_out(ctx, *st) ? 0 : -1;
}

void setbuf_(PPUContext& ctx, u32 file, u32 buf) { setvbuf_(ctx, file, buf, buf ? kIOFBF : kIONBF, 512); }

// perror: "s: message" (or the message alone for a null or empty s) and a newline on stderr.
void perror_(PPUContext& ctx, u32 s)
{
    const u32 code = get_errno(ctx);
    std::string text;
    if (s) {
        if (const auto p = read_string(thread_of(ctx), s); p && !p->empty()) text = *p + ": ";
    }
    text += error_text(code);
    text += '\n';
    put_text(ctx, "perror", std_file("_Stderr"), text);
}

// ------------------------------------------------------------------------------------------------- printf

class GuestFormat final : public FormatSource {
public:
    GuestFormat(PPUContext& ctx, VarArgs& args) : ctx_(ctx), args_(args) {}
    u64 next() override { return args_.next(); }
    std::optional<std::string> string(u32 addr, u64 max) override { return read_string(thread_of(ctx_), addr, max); }
    std::optional<std::u32string> wstring(u32 addr, u64 max) override
    {
        return read_wstring(thread_of(ctx_), addr, max);
    }
    bool store(u32 addr, u64 value, u32 bytes) override
    {
        if (!guest_range(thread_of(ctx_), addr, bytes)) return false;
        for (u32 k = 0; k < bytes; k++) gb(ctx_)[addr + k] = static_cast<u8>(value >> (8 * (bytes - 1 - k)));
        return true;
    }

private:
    PPUContext& ctx_;
    VarArgs& args_;
};

void note_problems(PPUContext& ctx, const char* fn, const std::vector<std::string>& problems)
{
    for (const std::string& p : problems) refuse(ctx, std::format("{}: {}", fn, p));
}

// The text of a narrow format: nullopt when the format is not memory or encodes badly (errno EILSEQ).
std::optional<std::string> narrow_text(PPUContext& ctx, const char* fn, u32 fmt, VarArgs& args)
{
    const auto f = read_string(thread_of(ctx), fmt);
    if (!f) {
        refuse(ctx, std::format("{}: the format at {:#x} runs out of guest memory", fn, fmt));
        return std::nullopt;
    }
    GuestFormat src(ctx, args);
    FormatResult<char> r = format<char>(*f, src);
    note_problems(ctx, fn, src.problems);
    if (r.failed) {
        set_errno(ctx, kEILSEQ);
        return std::nullopt;
    }
    return std::move(r.text);
}

std::optional<std::u32string> wide_text(PPUContext& ctx, const char* fn, u32 fmt, VarArgs& args)
{
    const auto f = read_wstring(thread_of(ctx), fmt, kMaxString / 4);
    if (!f) {
        refuse(ctx, std::format("{}: the wide format at {:#x} runs out of guest memory", fn, fmt));
        return std::nullopt;
    }
    GuestFormat src(ctx, args);
    FormatResult<char32_t> r = format<char32_t>(*f, src);
    note_problems(ctx, fn, src.problems);
    if (r.failed) {
        set_errno(ctx, kEILSEQ);
        return std::nullopt;
    }
    return std::move(r.text);
}

s32 print_to(PPUContext& ctx, const char* fn, u32 file, u32 fmt, VarArgs& args)
{
    const auto text = narrow_text(ctx, fn, fmt, args);
    if (!text) return -1;
    const auto st = stream_of(ctx, fn, file);
    if (!st) return -1;
    std::lock_guard l(st->lock);
    orient(*st, -1);
    return put_bytes(ctx, *st, *text) ? static_cast<s32>(text->size()) : -1;
}

s32 wprint_to(PPUContext& ctx, const char* fn, u32 file, u32 fmt, VarArgs& args)
{
    const auto text = wide_text(ctx, fn, fmt, args);
    if (!text) return -1;
    return put_wide(ctx, fn, file, *text) == 0 ? static_cast<s32>(text->size()) : -1;
}

s32 printf_(PPUContext& ctx, u32 fmt)
{
    VarArgs a(ctx, 1);
    return print_to(ctx, "printf", std_file("_Stdout"), fmt, a);
}

s32 fprintf_(PPUContext& ctx, u32 file, u32 fmt)
{
    VarArgs a(ctx, 2);
    return print_to(ctx, "fprintf", file, fmt, a);
}

s32 vprintf_(PPUContext& ctx, u32 fmt, u32 ap)
{
    VarArgs a = VarArgs::list(ctx, ap);
    return print_to(ctx, "vprintf", std_file("_Stdout"), fmt, a);
}

s32 vfprintf_(PPUContext& ctx, u32 file, u32 fmt, u32 ap)
{
    VarArgs a = VarArgs::list(ctx, ap);
    return print_to(ctx, "vfprintf", file, fmt, a);
}

// sprintf: the text and a NUL; its length.
s32 to_buffer(PPUContext& ctx, const char* fn, u32 buf, u32 fmt, VarArgs& args)
{
    const auto text = narrow_text(ctx, fn, fmt, args);
    if (!text) return -1;
    if (!range_ok(ctx, fn, buf, text->size() + 1)) return -1;
    write_string(thread_of(ctx), buf, *text);
    return static_cast<s32>(text->size());
}

// snprintf: at most n - 1 bytes and a NUL (nothing for n 0); the whole text's length.
s32 to_sized(PPUContext& ctx, const char* fn, u32 buf, u32 n, u32 fmt, VarArgs& args)
{
    const auto text = narrow_text(ctx, fn, fmt, args);
    if (!text) return -1;
    if (n) {
        const usize k = std::min<usize>(text->size(), n - 1);
        if (!range_ok(ctx, fn, buf, k + 1)) return -1;
        std::memcpy(gb(ctx) + buf, text->data(), k);
        gb(ctx)[buf + k] = 0;
    }
    return static_cast<s32>(text->size());
}

s32 sprintf_(PPUContext& ctx, u32 buf, u32 fmt)
{
    VarArgs a(ctx, 2);
    return to_buffer(ctx, "sprintf", buf, fmt, a);
}

s32 vsprintf_(PPUContext& ctx, u32 buf, u32 fmt, u32 ap)
{
    VarArgs a = VarArgs::list(ctx, ap);
    return to_buffer(ctx, "vsprintf", buf, fmt, a);
}

s32 snprintf_(PPUContext& ctx, u32 buf, u32 n, u32 fmt)
{
    VarArgs a(ctx, 3);
    return to_sized(ctx, "snprintf", buf, n, fmt, a);
}

s32 vsnprintf_(PPUContext& ctx, u32 buf, u32 n, u32 fmt, u32 ap)
{
    VarArgs a = VarArgs::list(ctx, ap);
    return to_sized(ctx, "vsnprintf", buf, n, fmt, a);
}

s32 wprintf_(PPUContext& ctx, u32 fmt)
{
    VarArgs a(ctx, 1);
    return wprint_to(ctx, "wprintf", std_file("_Stdout"), fmt, a);
}

s32 fwprintf_(PPUContext& ctx, u32 file, u32 fmt)
{
    VarArgs a(ctx, 2);
    return wprint_to(ctx, "fwprintf", file, fmt, a);
}

s32 vwprintf_(PPUContext& ctx, u32 fmt, u32 ap)
{
    VarArgs a = VarArgs::list(ctx, ap);
    return wprint_to(ctx, "vwprintf", std_file("_Stdout"), fmt, a);
}

s32 vfwprintf_(PPUContext& ctx, u32 file, u32 fmt, u32 ap)
{
    VarArgs a = VarArgs::list(ctx, ap);
    return wprint_to(ctx, "vfwprintf", file, fmt, a);
}

// swprintf: at most n wide characters with the NUL; negative when the text does not fit (C99 7.24.2.3), its first
// n - 1 characters written then.
s32 to_wide_sized(PPUContext& ctx, const char* fn, u32 buf, u32 n, u32 fmt, VarArgs& args)
{
    const auto text = wide_text(ctx, fn, fmt, args);
    if (!text) return -1;
    if (!n) return -1;
    const usize k = std::min<usize>(text->size(), n - 1);
    if (!range_ok(ctx, fn, buf, 4 * (k + 1))) return -1;
    for (usize i = 0; i < k; i++) ps_write32(gb(ctx), buf + 4 * static_cast<u32>(i), static_cast<u32>((*text)[i]));
    ps_write32(gb(ctx), buf + 4 * static_cast<u32>(k), 0);
    return text->size() < n ? static_cast<s32>(text->size()) : -1;
}

s32 swprintf_(PPUContext& ctx, u32 buf, u32 n, u32 fmt)
{
    VarArgs a(ctx, 3);
    return to_wide_sized(ctx, "swprintf", buf, n, fmt, a);
}

s32 vswprintf_(PPUContext& ctx, u32 buf, u32 n, u32 fmt, u32 ap)
{
    VarArgs a = VarArgs::list(ctx, ap);
    return to_wide_sized(ctx, "vswprintf", buf, n, fmt, a);
}

// ------------------------------------------------------------------------------------------------- scanf

class GuestTarget final : public ScanTarget {
public:
    GuestTarget(PPUContext& ctx, VarArgs& args) : ctx_(ctx), args_(args) {}
    u32 next_pointer() override { return static_cast<u32>(args_.next()); }
    bool put(u32 addr, const u8* data, usize size) override
    {
        if (!guest_range(thread_of(ctx_), addr, size)) return false;
        std::memcpy(gb(ctx_) + addr, data, size);
        return true;
    }

private:
    PPUContext& ctx_;
    VarArgs& args_;
};

template <class C>
class TextInput final : public ScanInput {
public:
    explicit TextInput(std::basic_string<C> s) : s_(std::move(s)) {}
    s64 get() override
    {
        const usize i = pos_++;
        return i < s_.size() ? static_cast<s64>(static_cast<std::make_unsigned_t<C>>(s_[i])) : -1;
    }
    void unget(s64) override
    {
        if (pos_) pos_--;
    }

private:
    std::basic_string<C> s_;
    usize pos_ = 0;
};

class StreamInput final : public ScanInput {
public:
    StreamInput(PPUContext& ctx, Stream& st) : ctx_(ctx), st_(st) {}
    s64 get() override { return get_byte(ctx_, st_); }
    void unget(s64 c) override
    {
        if (c >= 0) unget_byte(st_, static_cast<u8>(c));
    }

private:
    PPUContext& ctx_;
    Stream& st_;
};

template <class C>
s32 scan_text(PPUContext& ctx, const char* fn, std::basic_string<C> input, u32 fmt, VarArgs& args)
{
    std::optional<std::basic_string<C>> f;
    if constexpr (std::is_same_v<C, char>) f = read_string(thread_of(ctx), fmt);
    else f = read_wstring(thread_of(ctx), fmt, kMaxString / 4);
    if (!f) {
        refuse(ctx, std::format("{}: the format at {:#x} runs out of guest memory", fn, fmt));
        return -1;
    }
    TextInput<C> in(std::move(input));
    GuestTarget out(ctx, args);
    const int r = scan<C>(*f, in, out);
    note_problems(ctx, fn, out.problems);
    return r;
}

template <class C>
s32 scan_stream(PPUContext& ctx, const char* fn, u32 file, u32 fmt, VarArgs& args)
{
    std::optional<std::basic_string<C>> f;
    if constexpr (std::is_same_v<C, char>) f = read_string(thread_of(ctx), fmt);
    else f = read_wstring(thread_of(ctx), fmt, kMaxString / 4);
    if (!f) {
        refuse(ctx, std::format("{}: the format at {:#x} runs out of guest memory", fn, fmt));
        return -1;
    }
    const auto st = stream_of(ctx, fn, file);
    if (!st) return -1;
    std::lock_guard l(st->lock);
    orient(*st, std::is_same_v<C, char> ? -1 : 1);
    StreamInput in(ctx, *st);
    GuestTarget out(ctx, args);
    const int r = scan<C>(*f, in, out);
    note_problems(ctx, fn, out.problems);
    return r;
}

s32 sscanf_(PPUContext& ctx, u32 s, u32 fmt)
{
    VarArgs a(ctx, 2);
    const auto input = read_string(thread_of(ctx), s);
    if (!input) {
        refuse(ctx, std::format("sscanf: the string at {:#x} runs out of guest memory", s));
        return -1;
    }
    return scan_text<char>(ctx, "sscanf", *input, fmt, a);
}

s32 vsscanf_(PPUContext& ctx, u32 s, u32 fmt, u32 ap)
{
    VarArgs a = VarArgs::list(ctx, ap);
    const auto input = read_string(thread_of(ctx), s);
    if (!input) {
        refuse(ctx, std::format("vsscanf: the string at {:#x} runs out of guest memory", s));
        return -1;
    }
    return scan_text<char>(ctx, "vsscanf", *input, fmt, a);
}

s32 swscanf_(PPUContext& ctx, u32 s, u32 fmt)
{
    VarArgs a(ctx, 2);
    const auto input = read_wstring(thread_of(ctx), s, kMaxString / 4);
    if (!input) {
        refuse(ctx, std::format("swscanf: the wide string at {:#x} runs out of guest memory", s));
        return -1;
    }
    return scan_text<char32_t>(ctx, "swscanf", *input, fmt, a);
}

s32 vswscanf_(PPUContext& ctx, u32 s, u32 fmt, u32 ap)
{
    VarArgs a = VarArgs::list(ctx, ap);
    const auto input = read_wstring(thread_of(ctx), s, kMaxString / 4);
    if (!input) {
        refuse(ctx, std::format("vswscanf: the wide string at {:#x} runs out of guest memory", s));
        return -1;
    }
    return scan_text<char32_t>(ctx, "vswscanf", *input, fmt, a);
}

s32 scanf_(PPUContext& ctx, u32 fmt)
{
    VarArgs a(ctx, 1);
    return scan_stream<char>(ctx, "scanf", std_file("_Stdin"), fmt, a);
}

s32 vscanf_(PPUContext& ctx, u32 fmt, u32 ap)
{
    VarArgs a = VarArgs::list(ctx, ap);
    return scan_stream<char>(ctx, "vscanf", std_file("_Stdin"), fmt, a);
}

s32 fscanf_(PPUContext& ctx, u32 file, u32 fmt)
{
    VarArgs a(ctx, 2);
    return scan_stream<char>(ctx, "fscanf", file, fmt, a);
}

s32 vfscanf_(PPUContext& ctx, u32 file, u32 fmt, u32 ap)
{
    VarArgs a = VarArgs::list(ctx, ap);
    return scan_stream<char>(ctx, "vfscanf", file, fmt, a);
}

s32 wscanf_(PPUContext& ctx, u32 fmt)
{
    VarArgs a(ctx, 1);
    return scan_stream<char32_t>(ctx, "wscanf", std_file("_Stdin"), fmt, a);
}

s32 vwscanf_(PPUContext& ctx, u32 fmt, u32 ap)
{
    VarArgs a = VarArgs::list(ctx, ap);
    return scan_stream<char32_t>(ctx, "vwscanf", std_file("_Stdin"), fmt, a);
}

s32 fwscanf_(PPUContext& ctx, u32 file, u32 fmt)
{
    VarArgs a(ctx, 2);
    return scan_stream<char32_t>(ctx, "fwscanf", file, fmt, a);
}

s32 vfwscanf_(PPUContext& ctx, u32 file, u32 fmt, u32 ap)
{
    VarArgs a = VarArgs::list(ctx, ap);
    return scan_stream<char32_t>(ctx, "vfwscanf", file, fmt, a);
}

// ------------------------------------------------------------------------------------------------- files by name

std::optional<std::string> path_at(PPUContext& ctx, const char* fn, u32 path)
{
    auto p = read_string(thread_of(ctx), path);
    if (!p) refuse(ctx, std::format("{}: the path at {:#x} runs out of guest memory", fn, path));
    return p;
}

s32 lv2_result(PPUContext& ctx, lv2::Result r)
{
    if (r == lv2::CELL_OK) return 0;
    set_lv2_errno(ctx, r);
    return -1;
}

// remove: a file (unlink), or an empty directory (rmdir), as POSIX's remove.
s32 remove_(PPUContext& ctx, u32 path)
{
    if (!path_at(ctx, "remove", path)) return -1;
    lv2::PpuThread& t = thread_of(ctx);
    StackVar sb(ctx, t.kernel.base(), 64);
    if (lv2::sys_fs_stat(t, path, sb.addr()) == lv2::CELL_OK && (ps_read32(t.kernel.base(), sb.addr()) & 0040000))
        return lv2_result(ctx, lv2::sys_fs_rmdir(t, path));
    return lv2_result(ctx, lv2::sys_fs_unlink(t, path));
}

s32 rename_(PPUContext& ctx, u32 from, u32 to)
{
    if (!path_at(ctx, "rename", from) || !path_at(ctx, "rename", to)) return -1;
    return lv2_result(ctx, lv2::sys_fs_rename(thread_of(ctx), from, to));
}

// _wremove and _wrename: the wide paths' bytes (the "C" locale's), then remove and rename.
std::optional<std::string> wide_path(PPUContext& ctx, const char* fn, u32 path)
{
    const auto w = read_wstring(thread_of(ctx), path, 0x10000);
    if (!w) {
        refuse(ctx, std::format("{}: the wide path at {:#x} runs out of guest memory", fn, path));
        return std::nullopt;
    }
    std::string s;
    for (const char32_t c : *w) {
        const auto b = wide_to_byte(c);
        if (!b) {
            set_errno(ctx, kEILSEQ);
            return std::nullopt;
        }
        s += *b;
    }
    return s;
}

s32 _wremove_(PPUContext& ctx, u32 path)
{
    const auto p = wide_path(ctx, "_wremove", path);
    if (!p) return -1;
    StackVar buf(ctx, gb(ctx), static_cast<u32>(p->size() + 1));
    write_string(thread_of(ctx), buf.addr(), *p);
    return remove_(ctx, buf.addr());
}

s32 _wrename_(PPUContext& ctx, u32 from, u32 to)
{
    const auto a = wide_path(ctx, "_wrename", from), b = wide_path(ctx, "_wrename", to);
    if (!a || !b) return -1;
    StackVar buf(ctx, gb(ctx), static_cast<u32>(a->size() + b->size() + 2));
    write_string(thread_of(ctx), buf.addr(), *a);
    write_string(thread_of(ctx), buf.addr() + static_cast<u32>(a->size() + 1), *b);
    return rename_(ctx, buf.addr(), buf.addr() + static_cast<u32>(a->size() + 1));
}

// ------------------------------------------------------------------------------------------------- POSIX

// open(path, flags, mode): lv2's descriptor (3 and up), or -1.
s32 open_(PPUContext& ctx, u32 path, s32 flags, s32 mode)
{
    if (!path_at(ctx, "open", path)) return -1;
    s32 fd = -1;
    if (const lv2::Result r = Lv2Fs(ctx).open(path, flags, mode, &fd); r != lv2::CELL_OK) {
        set_lv2_errno(ctx, r);
        return -1;
    }
    Stdio& s = stdio();
    std::lock_guard g(s.mutex);
    s.fd_flags[fd] = flags;
    return fd;
}

s32 close_(PPUContext& ctx, s32 fd)
{
    if (fd >= 0 && fd <= kStderrFd) return 0;
    {
        Stdio& s = stdio();
        std::lock_guard g(s.mutex);
        s.fd_flags.erase(fd);
    }
    return lv2_result(ctx, Lv2Fs(ctx).close(fd));
}

// read: stdin has no input (0); stdout and stderr cannot be read.
s32 read_(PPUContext& ctx, s32 fd, u32 buf, u32 n)
{
    if (fd == kStdinFd) {
        flush_stdout(ctx);
        return 0;
    }
    if (fd == kStdoutFd || fd == kStderrFd) {
        set_errno(ctx, kEBADF);
        return -1;
    }
    if (!range_ok(ctx, "read", buf, n)) return -1;
    u64 got = 0;
    if (const lv2::Result r = Lv2Fs(ctx).read(fd, buf, n, &got); r != lv2::CELL_OK) {
        set_lv2_errno(ctx, r);
        return -1;
    }
    return static_cast<s32>(got);
}

// write: stdout and stderr to the TTY (stdout's buffered text first).
s32 write_(PPUContext& ctx, s32 fd, u32 buf, u32 n)
{
    if (!range_ok(ctx, "write", buf, n)) return -1;
    if (fd == kStdoutFd || fd == kStderrFd) {
        flush_stdout(ctx);
        tty_out(ctx, std::string_view(reinterpret_cast<const char*>(gb(ctx) + buf), n));
        return static_cast<s32>(n);
    }
    if (fd == kStdinFd) {
        set_errno(ctx, kEBADF);
        return -1;
    }
    u64 put = 0;
    if (const lv2::Result r = Lv2Fs(ctx).write(fd, buf, n, &put); r != lv2::CELL_OK) {
        set_lv2_errno(ctx, r);
        return -1;
    }
    return static_cast<s32>(put);
}

u64 seek_fd(PPUContext& ctx, s32 fd, s64 offset, s32 whence)
{
    if (fd >= 0 && fd <= kStderrFd) {
        set_errno(ctx, kESPIPE);
        return ~u64{0};
    }
    s64 pos = 0;
    if (const lv2::Result r = Lv2Fs(ctx).seek(fd, offset, whence, &pos); r != lv2::CELL_OK) {
        set_lv2_errno(ctx, r);
        return ~u64{0};
    }
    return static_cast<u64>(pos);
}

// lseek: off_t is 32 bits (EOVERFLOW past it); lseek64 the 64-bit form.
s32 lseek_(PPUContext& ctx, s32 fd, s32 offset, s32 whence)
{
    const u64 pos = seek_fd(ctx, fd, offset, whence);
    if (pos == ~u64{0}) return -1;
    if (pos > 0x7FFFFFFF) {
        set_errno(ctx, kEOVERFLOW);
        return -1;
    }
    return static_cast<s32>(pos);
}

u64 lseek64_(PPUContext& ctx, s32 fd, u64 offset, s32 whence) { return seek_fd(ctx, fd, static_cast<s64>(offset), whence); }

// stat and fstat fill lv2's CellFsStat (mode, uid, gid, then 64-bit times, size and block size at 4-byte alignment,
// 52 bytes): the C library's struct stat. The console's descriptors are character devices.
s32 stat_(PPUContext& ctx, u32 path, u32 sb)
{
    if (!path_at(ctx, "stat", path) || !range_ok(ctx, "stat", sb, 52)) return -1;
    return lv2_result(ctx, lv2::sys_fs_stat(thread_of(ctx), path, sb));
}

s32 fstat_(PPUContext& ctx, s32 fd, u32 sb)
{
    if (!range_ok(ctx, "fstat", sb, 52)) return -1;
    if (fd >= 0 && fd <= kStderrFd) {
        std::memset(gb(ctx) + sb, 0, 52);
        ps_write32(gb(ctx), sb, 0020000 | 0666); // S_IFCHR, rw for all
        ps_write64(gb(ctx), sb + 44, 512);
        return 0;
    }
    return lv2_result(ctx, lv2::sys_fs_fstat(thread_of(ctx), static_cast<u32>(fd), sb));
}

s32 unlink_(PPUContext& ctx, u32 path)
{
    if (!path_at(ctx, "unlink", path)) return -1;
    return lv2_result(ctx, lv2::sys_fs_unlink(thread_of(ctx), path));
}

s32 mkdir_(PPUContext& ctx, u32 path, s32 mode)
{
    if (!path_at(ctx, "mkdir", path)) return -1;
    return lv2_result(ctx, lv2::sys_fs_mkdir(thread_of(ctx), path, mode));
}

s32 rmdir_(PPUContext& ctx, u32 path)
{
    if (!path_at(ctx, "rmdir", path)) return -1;
    return lv2_result(ctx, lv2::sys_fs_rmdir(thread_of(ctx), path));
}

s32 chmod_(PPUContext& ctx, u32 path, s32 mode)
{
    if (!path_at(ctx, "chmod", path)) return -1;
    return lv2_result(ctx, lv2::sys_fs_chmod(thread_of(ctx), path, mode));
}

s32 truncate64_(PPUContext& ctx, u32 path, u64 size)
{
    if (!path_at(ctx, "truncate", path)) return -1;
    return lv2_result(ctx, lv2::sys_fs_truncate(thread_of(ctx), path, size));
}

s32 truncate_(PPUContext& ctx, u32 path, s32 size)
{
    if (size < 0) {
        set_errno(ctx, kEINVAL);
        return -1;
    }
    return truncate64_(ctx, path, static_cast<u64>(size));
}

s32 ftruncate64_(PPUContext& ctx, s32 fd, u64 size)
{
    return lv2_result(ctx, lv2::sys_fs_ftruncate(thread_of(ctx), static_cast<u32>(fd), size));
}

s32 ftruncate_(PPUContext& ctx, s32 fd, s32 size)
{
    if (size < 0) {
        set_errno(ctx, kEINVAL);
        return -1;
    }
    return ftruncate64_(ctx, fd, static_cast<u64>(size));
}

s32 fsync_(PPUContext& ctx, s32 fd)
{
    if (fd >= 0 && fd <= kStderrFd) return 0;
    return lv2_result(ctx, lv2::sys_fs_fsync(thread_of(ctx), static_cast<u32>(fd)));
}

// utime(path, times): lv2's CellFsUtimbuf (two 64-bit times); NULL is now.
s32 utime_(PPUContext& ctx, u32 path, u32 times)
{
    if (!path_at(ctx, "utime", path)) return -1;
    lv2::PpuThread& t = thread_of(ctx);
    if (times) {
        if (!range_ok(ctx, "utime", times, 16)) return -1;
        return lv2_result(ctx, lv2::sys_fs_utime(t, path, times));
    }
    StackVar now(ctx, t.kernel.base(), 16);
    lv2::sys_time_get_current_time(t, now.addr(), now.addr() + 8);
    ps_write64(t.kernel.base(), now.addr() + 8, ps_read64(t.kernel.base(), now.addr()));
    return lv2_result(ctx, lv2::sys_fs_utime(t, path, now.addr()));
}

// link: lv2's sys_fs_link (system call 810), its answer as it is.
s32 link_(PPUContext& ctx, u32 from, u32 to)
{
    if (!path_at(ctx, "link", from) || !path_at(ctx, "link", to)) return -1;
    return lv2_result(ctx, static_cast<lv2::Result>(lv2_syscall(ctx, 810, {from, to})));
}

// fcntl: F_GETFD/F_SETFD (no flags kept: 0), F_GETFL (open's flags), F_SETFL (O_APPEND as given); others EINVAL.
s32 fcntl_(PPUContext& ctx, s32 fd, s32 cmd, u32 arg)
{
    Stdio& s = stdio();
    std::lock_guard g(s.mutex);
    const bool console = fd >= 0 && fd <= kStderrFd;
    const auto it = s.fd_flags.find(fd);
    if (!console && it == s.fd_flags.end()) {
        set_errno(ctx, kEBADF);
        return -1;
    }
    switch (cmd) {
    case 1: return 0;
    case 2: return 0;
    case 3: return console ? (fd == kStdinFd ? kORdOnly : kOWrOnly) : it->second;
    case 4:
        if (!console) it->second = (it->second & ~kOAppend) | (static_cast<s32>(arg) & kOAppend);
        return 0;
    default: set_errno(ctx, kEINVAL); return -1;
    }
}

s32 getpid_(PPUContext& ctx) { return lv2::sys_process_getpid(thread_of(ctx)); }
s32 __getpid_(PPUContext& ctx) { return getpid_(ctx); }

// kill: lv2 has no signals.
s32 kill_(PPUContext& ctx, s32 pid, s32 sig)
{
    refuse(ctx, std::format("kill({}, {}): lv2 has no signals; ENOSYS", pid, sig));
    set_errno(ctx, kENOSYS);
    return -1;
}

s32 __rename_(PPUContext& ctx, u32 from, u32 to) { return rename_(ctx, from, to); }

// opendir, readdir, closedir: a DIR is the lv2 descriptor (+0) and the entry readdir returns (+8, CellFsDirent:
// type, name length, 256 bytes of name).
constexpr u32 kDirBytes = 8 + 258 + 6;

u32 opendir_(PPUContext& ctx, u32 path)
{
    if (!path_at(ctx, "opendir", path)) return 0;
    lv2::PpuThread& t = thread_of(ctx);
    StackVar out(ctx, t.kernel.base(), 8);
    if (const lv2::Result r = lv2::sys_fs_opendir(t, path, out.addr()); r != lv2::CELL_OK) {
        set_lv2_errno(ctx, r);
        return 0;
    }
    const u32 fd = ps_read32(t.kernel.base(), out.addr());
    const u32 dir = lib_malloc(ctx, kDirBytes);
    if (!dir) {
        lv2::sys_fs_closedir(t, fd);
        return 0;
    }
    std::memset(gb(ctx) + dir, 0, kDirBytes);
    ps_write32(gb(ctx), dir, fd);
    return dir;
}

u32 readdir_(PPUContext& ctx, u32 dir)
{
    if (!dir || !range_ok(ctx, "readdir", dir, kDirBytes)) {
        set_errno(ctx, kEBADF);
        return 0;
    }
    lv2::PpuThread& t = thread_of(ctx);
    StackVar n(ctx, t.kernel.base(), 8);
    if (const lv2::Result r = lv2::sys_fs_readdir(t, ps_read32(gb(ctx), dir), dir + 8, n.addr()); r != lv2::CELL_OK) {
        set_lv2_errno(ctx, r);
        return 0;
    }
    return ps_read64(t.kernel.base(), n.addr()) ? dir + 8 : 0;
}

s32 closedir_(PPUContext& ctx, u32 dir)
{
    if (!dir || !range_ok(ctx, "closedir", dir, kDirBytes)) {
        set_errno(ctx, kEBADF);
        return -1;
    }
    const s32 rc = lv2_result(ctx, lv2::sys_fs_closedir(thread_of(ctx), ps_read32(gb(ctx), dir)));
    lib_free(ctx, dir, "closedir");
    return rc;
}

// ------------------------------------------------------------------------------------------------- Dinkumware's

// FILE *_Fofind(void): a FILE for an open to fill (from malloc; _Foprep prepares it).
u32 _Fofind_(PPUContext& ctx)
{
    const u32 file = lib_malloc(ctx, kFileBytes);
    if (file) init_file(gb(ctx), file);
    return file;
}

// _FD_TYPE _Fopen(const char *path, unsigned int smode, const char *mods): the descriptor of `path` opened with
// Dinkumware's _M bits (_MOPENR, _MOPENW, _MOPENA, _MTRUNC, _MCREAT), or -1.
s32 _Fopen_(PPUContext& ctx, u32 path, u32 smode, u32)
{
    s32 flags = (smode & kMOpenR) && (smode & (kMOpenW | kMOpenA)) ? kORdWr
                : (smode & (kMOpenW | kMOpenA))                     ? kOWrOnly
                                                                     : kORdOnly;
    if (smode & kMOpenA) flags |= kOAppend;
    if (smode & kMTrunc) flags |= kOTrunc;
    if (smode & kMCreat) flags |= kOCreat;
    return open_(ctx, path, flags, 0666);
}

// FILE *_Foprep(const char *name, const char *mods, FILE *str, _FD_TYPE fd): the stream `str` prepared for `fd`
// with the mode `mods` (`name` opened with it when fd is negative); str, or NULL.
u32 _Foprep_(PPUContext& ctx, u32 name, u32 mods, u32 str, s32 fd)
{
    const auto o = mode_at(ctx, "_Foprep", mods);
    if (!o || !str || !range_ok(ctx, "_Foprep", str, kFileBytes)) return 0;
    if (fd < 0) {
        if (const lv2::Result r = Lv2Fs(ctx).open(name, o->flags, 0666, &fd); r != lv2::CELL_OK) {
            set_lv2_errno(ctx, r);
            return 0;
        }
    }
    const u32 file = make_stream(ctx, str, fd, *o);
    if (const auto st = find_stream(ctx, file)) st->owned = true; // _Fofind's FILE: freed at fclose
    return file;
}

// int _Frprep(FILE *str): 1 when bytes are there to read, 0 at the end, -1 on an error.
s32 _Frprep_(PPUContext& ctx, u32 file)
{
    const auto st = stream_of(ctx, "_Frprep", file);
    if (!st) return -1;
    std::lock_guard l(st->lock);
    if (!st->back.empty() || st->in_pos < st->in.size()) return 1;
    if (fill(ctx, *st)) return 1;
    return st->error ? -1 : 0;
}

// int _Fwprep(FILE *str): 0 when the stream may be written, -1 otherwise.
s32 _Fwprep_(PPUContext& ctx, u32 file)
{
    const auto st = stream_of(ctx, "_Fwprep", file);
    if (!st) return -1;
    std::lock_guard l(st->lock);
    return to_write(ctx, *st) ? 0 : -1;
}

s32 _WFrprep_(PPUContext& ctx, u32 file) { return _Frprep_(ctx, file); }
s32 _WFwprep_(PPUContext& ctx, u32 file) { return _Fwprep_(ctx, file); }

// long _Fgpos(FILE *str, fpos_t *ptr): fgetpos's (0) with ptr, ftell's (the position) without.
s32 _Fgpos_(PPUContext& ctx, u32 file, u32 ptr) { return ptr ? fgetpos_(ctx, file, ptr) : ftell_(ctx, file); }

// int _Fspos(FILE *str, const fpos_t *ptr, long loff, int way): fsetpos's with ptr, fseek's without.
s32 _Fspos_(PPUContext& ctx, u32 file, u32 ptr, s32 loff, s32 way)
{
    if (!ptr) return fseek_(ctx, file, loff, way);
    if (!range_ok(ctx, "_Fspos", ptr, 16)) return -1;
    const auto st = stream_of(ctx, "_Fspos", file);
    if (!st) return -1;
    std::lock_guard l(st->lock);
    return seek(ctx, *st, static_cast<s64>(ps_read64(gb(ctx), ptr)) + loff, kSeekSet);
}

void _Lockfilelock_(PPUContext& ctx, u32 file)
{
    if (const auto st = stream_of(ctx, "_Lockfilelock", file)) st->lock.lock();
}

void _Unlockfilelock_(PPUContext& ctx, u32 file)
{
    if (const auto st = stream_of(ctx, "_Unlockfilelock", file)) st->lock.unlock();
}

// Sony's: the stdio set-up and shut-down its crt calls, closing every stream (_close_all_FILE, _fs_finalize) or one
// (_filep_close_it, fclose's work).
s32 _fs_initialize_(PPUContext& ctx)
{
    Stdio& s = stdio();
    std::lock_guard g(s.mutex);
    ensure_std(ctx, s);
    return 0;
}

void close_all(PPUContext& ctx)
{
    std::vector<std::shared_ptr<Stream>> all;
    {
        Stdio& s = stdio();
        std::lock_guard g(s.mutex);
        ensure_std(ctx, s);
        for (const auto& [f, st] : s.streams) all.push_back(st);
    }
    for (const auto& st : all) {
        if (st->console) flush_stream(ctx, *st);
        else close_stream(ctx, st);
    }
}

s32 _fs_finalize_(PPUContext& ctx)
{
    close_all(ctx);
    return 0;
}

void _close_all_FILE_(PPUContext& ctx) { close_all(ctx); }
s32 _filep_close_it_(PPUContext& ctx, u32 file) { return fclose_(ctx, file); }

// void _Vacopy(va_list *dst, va_list src): a PPC64 va_list is a pointer, copied.
void _Vacopy_(PPUContext& ctx, u32 dst, u32 src)
{
    if (range_ok(ctx, "_Vacopy", dst, 4)) ps_write32(gb(ctx), dst, src);
}

void std_file_init(u8* g, u32 file) { init_file(g, file); }
void zero_init(u8*, u32) {}

} // namespace

namespace libc {
// Every output stream flushed (exit's last step before lv2 ends the process).
void stdio_flush_all(PPUContext& ctx) { fflush_(ctx, 0); }
} // namespace libc

#define LIBC(name) ::pspocket::hle::Function{{}, #name, ::pspocket::recomp::nid_of(#name), &detail::thunk<&name##_>, false}
#define LIBC_AS(name, fn) ::pspocket::hle::Function{{}, #name, ::pspocket::recomp::nid_of(#name), &detail::thunk<&fn##_>, false}

void register_sys_libc_stdio(Registry& r)
{
    for (const std::string_view lib : {std::string_view("sys_libc"), std::string_view("sys_libm")}) {
        r.add(lib, {
            LIBC(fopen), LIBC(freopen), LIBC(fdopen), LIBC(fclose), LIBC(tmpnam), LIBC(tmpfile), LIBC(fgetc),
            LIBC_AS(getc, fgetc), LIBC(getchar), LIBC(fgets), LIBC(gets), LIBC(fputc), LIBC_AS(putc, fputc),
            LIBC(putchar), LIBC(fputs), LIBC(puts), LIBC(ungetc), LIBC(fread), LIBC(fwrite), LIBC(fgetwc),
            LIBC_AS(getwc, fgetwc), LIBC(getwchar), LIBC(fputwc), LIBC_AS(putwc, fputwc), LIBC(putwchar),
            LIBC(fgetws), LIBC(fputws), LIBC(ungetwc), LIBC(fwide), LIBC(fseek), LIBC(ftell), LIBC(rewind),
            LIBC(fgetpos), LIBC(fsetpos), LIBC(feof), LIBC(ferror), LIBC(clearerr), LIBC(fileno), LIBC(fflush),
            LIBC(setvbuf), LIBC(setbuf), LIBC(perror), LIBC(printf), LIBC(fprintf), LIBC(vprintf), LIBC(vfprintf),
            LIBC(sprintf), LIBC(vsprintf), LIBC(snprintf), LIBC(vsnprintf), LIBC(wprintf), LIBC(fwprintf),
            LIBC(vwprintf), LIBC(vfwprintf), LIBC(swprintf), LIBC(vswprintf), LIBC(sscanf), LIBC(vsscanf),
            LIBC(swscanf), LIBC(vswscanf), LIBC(scanf), LIBC(vscanf), LIBC(fscanf), LIBC(vfscanf), LIBC(wscanf),
            LIBC(vwscanf), LIBC(fwscanf), LIBC(vfwscanf), LIBC(remove), LIBC(rename), LIBC(_wremove),
            LIBC(_wrename), LIBC(__rename), LIBC(open), LIBC(close), LIBC(read), LIBC(write), LIBC(lseek),
            LIBC(lseek64), LIBC(stat), LIBC(fstat), LIBC(unlink), LIBC(mkdir), LIBC(rmdir), LIBC(chmod),
            LIBC(truncate), LIBC(truncate64), LIBC(ftruncate), LIBC(ftruncate64), LIBC(fsync), LIBC(utime),
            LIBC(link), LIBC(fcntl), LIBC(getpid), LIBC(__getpid), LIBC(kill), LIBC(opendir), LIBC(readdir),
            LIBC(closedir), LIBC(_Fofind), LIBC(_Fopen), LIBC(_Foprep), LIBC(_Frprep), LIBC(_Fwprep),
            LIBC(_WFrprep), LIBC(_WFwprep), LIBC(_Fgpos), LIBC(_Fspos), LIBC(_Lockfilelock), LIBC(_Unlockfilelock),
            LIBC(_fs_initialize), LIBC(_fs_finalize), LIBC(_close_all_FILE), LIBC(_filep_close_it), LIBC(_Vacopy),
        });
    }
    r.add_variables("sys_libc", {
        Variable{{}, "_Stdin", recomp::nid_of("_Stdin"), kFileBytes, 8, &std_file_init},
        Variable{{}, "_Stdout", recomp::nid_of("_Stdout"), kFileBytes, 8, &std_file_init},
        Variable{{}, "_Stderr", recomp::nid_of("_Stderr"), kFileBytes, 8, &std_file_init},
        Variable{{}, "_FILE_P_Head", recomp::nid_of("_FILE_P_Head"), 4, 4, &zero_init},
    });
}
#undef LIBC_AS
#undef LIBC

} // namespace pspocket::hle
