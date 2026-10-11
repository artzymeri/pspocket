#pragma once

// sys_libc's guest side (D-508): checked guest memory, the variadic arguments of a call, errno and the per-thread
// block the C library keeps for each lv2 thread. Included by sys_libc.cpp (and its other libc_*.hpp parts) only.
//
// Throughout sys_libc: a guest `char` is unsigned, `int`, `long`, `size_t` and pointers are 32 bits, `long long` and
// `time_t` 64, `wchar_t` is a 32-bit big-endian word, `long double` is `double`. Guest pointers are checked before
// they are used (D-042): a range that is not committed guest memory is refused with the call site logged.

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/syscalls.hpp"

namespace pspocket::hle::libc {

inline constexpr std::string_view kChan = "sys_libc";

// errno values: the C library's errno.h has lv2's codes (CELL_EDOM and CELL_ERANGE exist for it; CELL_EFPOS is
// Dinkumware's EFPOS), and the file functions leave lv2's own errors in errno.
inline constexpr u32 kEDOM = lv2::CELL_EDOM, kERANGE = lv2::CELL_ERANGE, kEILSEQ = lv2::CELL_EILSEQ,
                     kEFPOS = lv2::CELL_EFPOS, kEINVAL = lv2::CELL_EINVAL, kENOMEM = lv2::CELL_ENOMEM,
                     kEBADF = lv2::CELL_EBADF, kEFAULT = lv2::CELL_EFAULT, kENOSYS = lv2::CELL_ENOSYS,
                     kEOVERFLOW = lv2::CELL_EOVERFLOW, kESPIPE = lv2::CELL_ESPIPE, kEMFILE = lv2::CELL_EMFILE,
                     kENOTSUP = lv2::CELL_ENOTSUP, kEIO = lv2::CELL_EIO, kEEXIST = lv2::CELL_EEXIST;

// How far a string the library reads (a format, a %s argument, a path) may go.
inline constexpr u64 kMaxString = 0x1000000;
inline constexpr u64 kNoLimit = GuestMemory::kGuestSize;

inline u64 host_page() { return GuestMemory::host_page_size(); }

// Whether [addr, addr + size) is committed guest memory, page by page.
inline bool guest_range(lv2::PpuThread& t, u32 addr, u64 size)
{
    if (size == 0) return true;
    if (u64{addr} + size > GuestMemory::kGuestSize) return false;
    const u64 page = host_page();
    for (u64 p = addr & ~(page - 1); p < u64{addr} + size; p += page)
        if (!t.kernel.memory().is_committed(static_cast<u32>(p))) return false;
    return true;
}

// The length of the guest string at `addr`, looking at `max` bytes at most: nullopt when memory that is not committed
// comes before its NUL.
inline std::optional<u64> guest_strnlen(lv2::PpuThread& t, u32 addr, u64 max)
{
    u8* base = t.kernel.base();
    const u64 page = host_page();
    // `max` may be any size ("no limit"): the search stops at the end of guest memory, a string that runs into it
    // being no string.
    const u64 room = GuestMemory::kGuestSize - addr;
    const bool capped = max <= room;
    const u64 end = u64{addr} + (capped ? max : room);
    for (u64 a = addr; a < end;) {
        if (!t.kernel.memory().is_committed(static_cast<u32>(a))) return std::nullopt;
        const u64 stop = std::min(end, (a & ~(page - 1)) + page);
        if (const void* nul = std::memchr(base + a, 0, stop - a)) return static_cast<const u8*>(nul) - (base + addr);
        a = stop;
    }
    if (!capped) return std::nullopt;
    return max;
}

// Guest bytes read in order from `addr`, each host page checked once; -1 for one that is not committed guest memory.
class GuestBytes {
public:
    GuestBytes(lv2::PpuThread& t, u32 addr) : memory_(t.kernel.memory()), base_(t.kernel.base()), addr_(addr) {}

    int at(u64 i)
    {
        const u64 a = u64{addr_} + i;
        if (a < begin_ || a >= end_) {
            if (a >= GuestMemory::kGuestSize || !memory_.is_committed(static_cast<u32>(a))) return -1;
            begin_ = a & ~(host_page() - 1);
            end_ = begin_ + host_page();
        }
        return base_[a];
    }

private:
    GuestMemory& memory_;
    u8* base_;
    u32 addr_;
    u64 begin_ = 1, end_ = 0; // the page known committed
};

// Guest wchar_t (32-bit big-endian words) read in order from `addr`; -1 (as s64) past committed memory.
class GuestWords {
public:
    GuestWords(lv2::PpuThread& t, u32 addr) : bytes_(t, addr), base_(t.kernel.base()), addr_(addr) {}

    s64 at(u64 i)
    {
        if (bytes_.at(4 * i) < 0 || bytes_.at(4 * i + 3) < 0) return -1;
        return ps_read32(base_, static_cast<u32>(addr_ + 4 * i));
    }

private:
    GuestBytes bytes_;
    u8* base_;
    u32 addr_;
};

// The guest string at `addr` (at most `max` bytes), or nullopt (memory that is not committed comes first).
inline std::optional<std::string> read_string(lv2::PpuThread& t, u32 addr, u64 max = kMaxString)
{
    if (!addr) return std::nullopt;
    const std::optional<u64> n = guest_strnlen(t, addr, max);
    if (!n) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(t.kernel.base() + addr), *n);
}

// The guest wide string at `addr` (at most `max` characters), or nullopt.
inline std::optional<std::u32string> read_wstring(lv2::PpuThread& t, u32 addr, u64 max = kMaxString / 4)
{
    if (!addr) return std::nullopt;
    GuestWords w(t, addr);
    std::u32string s;
    for (u64 i = 0; i < max; i++) {
        const s64 c = w.at(i);
        if (c < 0) return std::nullopt;
        if (c == 0) break;
        s += static_cast<char32_t>(c);
    }
    return s;
}

// Writes `s` and a NUL at `addr` when it is guest memory.
inline bool write_string(lv2::PpuThread& t, u32 addr, std::string_view s)
{
    if (!guest_range(t, addr, s.size() + 1)) return false;
    std::memcpy(t.kernel.base() + addr, s.data(), s.size());
    t.kernel.base()[addr + s.size()] = 0;
    return true;
}

// Writes the wide characters of `s` (and a NUL when `nul`) at `addr`.
inline bool write_wstring(lv2::PpuThread& t, u32 addr, std::u32string_view s, bool nul = true)
{
    if (!guest_range(t, addr, 4 * (u64{s.size()} + (nul ? 1 : 0)))) return false;
    u8* base = t.kernel.base();
    for (usize i = 0; i < s.size(); i++) ps_write32(base, static_cast<u32>(addr + 4 * i), static_cast<u32>(s[i]));
    if (nul) ps_write32(base, static_cast<u32>(addr + 4 * s.size()), 0);
    return true;
}

// A call refused because a pointer does not lead to guest memory (D-042): logged with its call site.
inline void refuse(PPUContext& ctx, std::string_view why) { log_call_site(ctx, guest_base(ctx), why); }

// The messages of strerror and perror (Dinkumware's _Strerror) for the C library's errno codes, which are lv2's: its
// own (EDOM, ERANGE, EILSEQ, EFPOS) and those the file functions leave from lv2.
inline std::string_view error_text(u32 code)
{
    switch (code) {
    case 0: return "no error";
    case lv2::CELL_EDOM: return "domain error";
    case lv2::CELL_ERANGE: return "range error";
    case lv2::CELL_EILSEQ: return "invalid multibyte sequence";
    case lv2::CELL_EFPOS: return "invalid file position";
    case lv2::CELL_EAGAIN: return "resource temporarily unavailable";
    case lv2::CELL_EINVAL: return "invalid argument";
    case lv2::CELL_ENOSYS: return "function not implemented";
    case lv2::CELL_ENOMEM: return "not enough memory";
    case lv2::CELL_ESRCH: return "no such object";
    case lv2::CELL_ENOENT: return "no such file or directory";
    case lv2::CELL_ENOEXEC: return "exec format error";
    case lv2::CELL_EDEADLK: return "resource deadlock avoided";
    case lv2::CELL_EPERM: return "operation not permitted";
    case lv2::CELL_EBUSY: return "device or resource busy";
    case lv2::CELL_ETIMEDOUT: return "timed out";
    case lv2::CELL_EFAULT: return "bad address";
    case lv2::CELL_EISDIR: return "is a directory";
    case lv2::CELL_EEXIST: return "file exists";
    case lv2::CELL_EINTR: return "interrupted";
    case lv2::CELL_EFBIG: return "file too large";
    case lv2::CELL_ENFILE: return "too many open files in the system";
    case lv2::CELL_ENOSPC: return "no space left on device";
    case lv2::CELL_ENOTTY: return "not a terminal";
    case lv2::CELL_EPIPE: return "broken pipe";
    case lv2::CELL_EROFS: return "read-only file system";
    case lv2::CELL_ESPIPE: return "invalid seek";
    case lv2::CELL_EACCES: return "permission denied";
    case lv2::CELL_EBADF: return "bad file descriptor";
    case lv2::CELL_EIO: return "input/output error";
    case lv2::CELL_EMFILE: return "too many open files";
    case lv2::CELL_ENODEV: return "no such device";
    case lv2::CELL_ENOTDIR: return "not a directory";
    case lv2::CELL_EXDEV: return "cross-device link";
    case lv2::CELL_ENAMETOOLONG: return "file name too long";
    case lv2::CELL_ENOTEMPTY: return "directory not empty";
    case lv2::CELL_ENOTSUP: return "not supported";
    case lv2::CELL_EOVERFLOW: return "value too large";
    case lv2::CELL_ENOTMOUNTED: return "file system not mounted";
    default: return "unknown error";
    }
}

// Whether [addr, addr + size) is guest memory; refused (logged) for `fn` when it is not.
inline bool range_ok(PPUContext& ctx, std::string_view fn, u32 addr, u64 size)
{
    if (guest_range(thread_of(ctx), addr, size)) return true;
    refuse(ctx, std::format("{}: [{:#x}, +{:#x}) is not guest memory", fn, addr, size));
    return false;
}

// Text to the TTY's channel 0 through sys_tty_write, as liblv2's printf writes (sysPrxForUser_libc.cpp tty_print):
// in pieces of up to 1 KB on the caller's stack, each ending after a line where one ends in it.
inline void tty_out(PPUContext& ctx, std::string_view text)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    constexpr usize kPiece = 0x400;
    std::string_view rest = text;
    while (!rest.empty()) {
        usize n = std::min(rest.size(), kPiece);
        if (n < rest.size()) {
            if (const usize nl = rest.rfind('\n', n - 1); nl != std::string_view::npos) n = nl + 1;
        }
        StackVar piece(ctx, base, kPiece + 8);
        std::memcpy(base + piece.addr(), rest.data(), n);
        lv2::sys_tty_write(t, 0, piece.addr(), static_cast<u32>(n), piece.addr() + kPiece);
        rest.remove_prefix(n);
    }
}

// The variadic arguments of a call after its `fixed` named ones, as the PPC64 ELF ABI passes them (RPCS3
// ps3_fmt_src): one 64-bit slot each, argument n in r(3 + n) for n < 8, else in the caller's parameter save area at
// r1 + 0x30 + 8n. A double is in its slot as well (a variadic call puts it there besides an FPR). Or those of a
// va_list (list()): the ABI's va_list is the address of the next argument's 64-bit slot, so argument n is at ap + 8n.
// Construct it before any StackVar moves r1.
class VarArgs {
public:
    VarArgs(PPUContext& ctx, u32 fixed) : ctx_(ctx), sp_(static_cast<u32>(ctx.gpr[1])), next_(fixed) {}

    static VarArgs list(PPUContext& ctx, u32 ap)
    {
        VarArgs a(ctx, 0);
        a.list_ = ap;
        a.from_list_ = true;
        return a;
    }

    u64 next()
    {
        const u32 n = next_++;
        u64 slot = 0;
        if (from_list_) slot = u64{list_} + 8 * u64{n};
        else if (n < 8) return ctx_.gpr[3 + n];
        else slot = u64{sp_} + 0x30 + 8 * u64{n};
        if (slot + 8 > GuestMemory::kGuestSize || !guest_readable(thread_of(ctx_), static_cast<u32>(slot), 8)) {
            unreadable_ = true;
            return 0;
        }
        return ps_read64(guest_base(ctx_), static_cast<u32>(slot));
    }
    // A vector argument (Sony's printf qualifier v): two slots at a 16-byte boundary of the argument area (slot n is
    // at r1 + 0x30 + 8n, so an even one; a va_list's address rounded up to 16, as the SDK's va_arg), in memory order.
    std::array<u8, 16> next_vector()
    {
        if (from_list_ ? ((list_ + 8 * next_) & 15) != 0 : (next_ & 1) != 0) next_++;
        std::array<u8, 16> v{};
        ps_write64(v.data(), 0, next());
        ps_write64(v.data(), 8, next());
        return v;
    }
    bool unreadable() const { return unreadable_; }
    // The va_list's address after the arguments taken so far (list() only).
    u32 position() const { return static_cast<u32>(list_ + 8 * next_); }

private:
    PPUContext& ctx_;
    const u32 sp_;
    u32 next_;
    u32 list_ = 0;
    bool from_list_ = false;
    bool unreadable_ = false;
};

// ------------------------------------------------------------------------------------------------- per-thread block

// What the library keeps for each lv2 thread, in guest memory (the C library's per-thread statics, thread-local in the
// SDK's): errno, the conversion states the standard functions keep between calls, strtok's position, the static
// results of gmtime, localtime, asctime and strerror, the global locale pointer and the four state tables (D-906).
// 512 bytes, in blocks of 64 KB of the main area.
namespace tb {
inline constexpr u32 kErrno = 0;
inline constexpr u32 kMbState = 8;     // mbtowc's internal state (mbstate_t, 8 bytes)
inline constexpr u32 kWcState = 16;    // wctomb's
inline constexpr u32 kCoState = 24;    // mblen's
inline constexpr u32 kWcoState = 32;   // (spare)
inline constexpr u32 kMbrlen = 40;     // mbrlen's internal state
inline constexpr u32 kMbrtowc = 48;    // mbrtowc's
inline constexpr u32 kWcrtomb = 56;    // wcrtomb's
inline constexpr u32 kMbsrtowcs = 64;  // mbsrtowcs's
inline constexpr u32 kWcsrtombs = 72;  // wcsrtombs's
inline constexpr u32 kStrtok = 80;     // strtok's position (u32)
inline constexpr u32 kWcstok = 84;     // (spare)
inline constexpr u32 kGlobalLocale = 88; // _Getgloballocale's pointer (u32)
inline constexpr u32 kTm = 96;         // struct tm (36 bytes): gmtime, localtime
inline constexpr u32 kAsctime = 136;   // asctime and ctime: 26 bytes
inline constexpr u32 kStrerror = 168;  // strerror's message: 80 bytes
inline constexpr u32 kMbTab = 256;     // _Getpmbstate: the multibyte state tables (_Statab, 64 bytes)
inline constexpr u32 kWcTab = 320;     // _Getpwcstate: the wide ones
inline constexpr u32 kCoTab = 384;     // _Getpcostate: the collation ones
inline constexpr u32 kWcoTab = 448;    // _Getpwcostate: the wide collation ones
inline constexpr u32 kSize = 512;
} // namespace tb

struct ThreadBlocks {
    std::mutex mutex;
    std::unordered_map<u32, u32> by_thread; // lv2 thread ID -> block
    u32 chunk = 0, used = 0;                // the 64 KB of the main area being handed out
};

// The calling thread's block (0 when the main area has no 64 KB left).
inline u32 thread_block(PPUContext& ctx)
{
    lv2::PpuThread& t = thread_of(ctx);
    HleProcess* p = HleProcess::current();
    ThreadBlocks& s = p->state<ThreadBlocks>();
    std::lock_guard lock(s.mutex);
    if (const auto it = s.by_thread.find(t.id); it != s.by_thread.end()) return it->second;
    if (!s.chunk || s.used + tb::kSize > 0x10000) {
        s.chunk = p->main_memory().alloc(0x10000, 0x10000);
        s.used = 0;
        if (!s.chunk) {
            PS_LOG_ERROR(kChan, "no main-area memory for thread {:#x}'s C library block", t.id);
            return 0;
        }
    }
    const u32 b = s.chunk + s.used;
    s.used += tb::kSize;
    std::memset(t.kernel.base() + b, 0, tb::kSize);
    s.by_thread.emplace(t.id, b);
    return b;
}

inline void set_errno(PPUContext& ctx, u32 code)
{
    if (const u32 b = thread_block(ctx)) ps_write32(guest_base(ctx), b + tb::kErrno, code);
}

inline u32 get_errno(PPUContext& ctx)
{
    const u32 b = thread_block(ctx);
    return b ? ps_read32(guest_base(ctx), b + tb::kErrno) : 0;
}

// Every output stream of stdio flushed (sys_libc_stdio.cpp; exit's last step before lv2 ends the process).
void stdio_flush_all(PPUContext& ctx);

// The guest address of a host variable of sys_libc (laid out the first time, D-503).
inline u32 variable(u32 nid) { return HleProcess::current()->link_variable("sys_libc", nid); }

} // namespace pspocket::hle::libc
