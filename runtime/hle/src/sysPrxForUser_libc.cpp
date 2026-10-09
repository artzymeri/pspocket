// sysPrxForUser's C library: liblv2's memory and string functions, ctype, malloc, the printf family and qsort
// (RPCS3 rpcs3/Emu/Cell/Modules/sys_libc_.cpp, its formatter Utilities/cfmt.h), and its heap functions (sys_heap.cpp).
// Firmware modules import them, every one of them (D-166); games call some. Each function is RPCS3's; what differs is
// written next to it and in D-168. Throughout:
// - Guest pointers are checked before they are used (D-042): a range that is not committed guest memory is refused
//   with the call site logged, where RPCS3 would fault. Scans (strlen, memchr, memcmp, ...) check each page as they
//   reach it, so they fail only where RPCS3's would.
// - A guest `char` is unsigned, as the PPU ABI has it: bytes from 0x80 compare above ASCII.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "guest.hpp"
#include "idm.hpp"
#include "lv2_heap.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sysPrxForUser";
using lv2::err;

// ------------------------------------------------------------------------------------------------- guest memory

constexpr u64 kNoLimit = GuestMemory::kGuestSize;
// How far a string a format reads (the format, a %s argument) may go: past it the text is cut.
constexpr u64 kMaxString = 0x1000000;

u64 host_page() { return GuestMemory::host_page_size(); }

// Whether [addr, addr + size) is committed guest memory, page by page (guest_readable covers one page at most).
bool guest_range(lv2::PpuThread& t, u32 addr, u64 size)
{
    if (size == 0) return true;
    if (u64{addr} + size > GuestMemory::kGuestSize) return false;
    const u64 page = host_page();
    for (u64 p = addr & ~(page - 1); p < u64{addr} + size; p += page)
        if (!t.kernel.memory().is_committed(static_cast<u32>(p))) return false;
    return true;
}

// The length of the guest string at `addr`, looking at `max` bytes at most (a string without a NUL in them has length
// `max`): nullopt when memory that is not committed comes before its NUL.
std::optional<u64> guest_strnlen(lv2::PpuThread& t, u32 addr, u64 max)
{
    u8* base = t.kernel.base();
    const u64 page = host_page();
    const u64 end = u64{addr} + max;
    for (u64 a = addr; a < end;) {
        if (a >= GuestMemory::kGuestSize || !t.kernel.memory().is_committed(static_cast<u32>(a))) return std::nullopt;
        const u64 stop = std::min(end, (a & ~(page - 1)) + page);
        if (const void* nul = std::memchr(base + a, 0, stop - a)) return static_cast<const u8*>(nul) - (base + addr);
        a = stop;
    }
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

// A call refused because a pointer does not lead to guest memory (D-042).
void refuse(PPUContext& ctx, std::string_view why) { log_call_site(ctx, guest_base(ctx), why); }

// ------------------------------------------------------------------------------------------------- ctype

// RPCS3 sys_libc_.cpp s_ctype_table: the class bits of EOF (-1) and of 0..127: 1 upper case, 2 lower case, 4 digit,
// 8 white space, 0x10 punctuation and the space character, 0x20 control, 0x40 hex digit letter, 0x400 with TAB.
constexpr std::array<u16, 129> kCtype = {
    0,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x408,
    8, 8, 8, 8,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x18,
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x42, 0x42, 0x42, 0x42, 0x42, 0x42,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    0x10, 0x10, 0x10, 0x10, 0x20,
};

struct CtypeState {
    std::atomic<bool> logged{false};
};

// RPCS3 aborts on a character outside the table (its ensure(ch >= -1 && ch <= 127)); here it has no class and no case:
// logged at warning the first time in a process, then at debug.
bool in_ctype_table(PPUContext& ctx, s32 ch, std::string_view what)
{
    if (ch >= -1 && ch <= 127) return true;
    HleProcess* p = HleProcess::current();
    const bool first = p && !p->state<CtypeState>().logged.exchange(true);
    log::print(first ? log::Level::Warn : log::Level::Debug, kChan,
        "{}({}) at {:#010x}: not in the table (-1..127, where RPCS3 aborts); no class, no case", what, ch,
        static_cast<u32>(ctx.lr) - 4);
    return false;
}

s32 __sys_look_ctype_table(PPUContext& ctx, s32 ch)
{
    return in_ctype_table(ctx, ch, "__sys_look_ctype_table") ? kCtype[ch + 1] : 0;
}

s32 _sys_tolower(PPUContext& ctx, s32 ch)
{
    if (!in_ctype_table(ctx, ch, "_sys_tolower")) return ch;
    return (kCtype[ch + 1] & 1) ? ch + 0x20 : ch;
}

s32 _sys_toupper(PPUContext& ctx, s32 ch)
{
    if (!in_ctype_table(ctx, ch, "_sys_toupper")) return ch;
    return (kCtype[ch + 1] & 2) ? ch - 0x20 : ch;
}

// A byte as _sys_strncasecmp folds it: 'A'..'Z' to lower case, the rest as they are.
int fold(int c) { return c >= 0 && c <= 127 && (kCtype[c + 1] & 1) ? c + 0x20 : c; }

// ------------------------------------------------------------------------------------------------- memory

u32 _sys_memset(PPUContext& ctx, u32 dst, s32 value, u32 size)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_range(t, dst, size)) {
        refuse(ctx, std::format("_sys_memset({:#x}, {}, {:#x}): not guest memory; nothing written", dst, value, size));
        return dst;
    }
    std::memset(t.kernel.base() + dst, value, size);
    return dst;
}

// RPCS3 _sys_memcpy (std::memcpy) and _sys_memmove (std::memmove): both copy as memmove here, so overlapping ranges,
// which memcpy leaves undefined, come out as memmove's.
u32 copy_memory(PPUContext& ctx, std::string_view what, u32 dst, u32 src, u32 size)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_range(t, dst, size) || !guest_range(t, src, size)) {
        refuse(ctx, std::format("{}({:#x}, {:#x}, {:#x}): not guest memory; nothing copied", what, dst, src, size));
        return dst;
    }
    std::memmove(t.kernel.base() + dst, t.kernel.base() + src, size);
    return dst;
}

u32 _sys_memcpy(PPUContext& ctx, u32 dst, u32 src, u32 size) { return copy_memory(ctx, "_sys_memcpy", dst, src, size); }

u32 _sys_memmove(PPUContext& ctx, u32 dst, u32 src, u32 size)
{
    return copy_memory(ctx, "_sys_memmove", dst, src, size);
}

// RPCS3: -1, 0 or 1 from the first byte that differs (unsigned).
s32 _sys_memcmp(PPUContext& ctx, u32 buf1, u32 buf2, u32 size)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u64 page = host_page();
    for (u64 i = 0; i < size;) {
        const u64 a = u64{buf1} + i, b = u64{buf2} + i;
        if (a >= GuestMemory::kGuestSize || b >= GuestMemory::kGuestSize ||
            !t.kernel.memory().is_committed(static_cast<u32>(a)) ||
            !t.kernel.memory().is_committed(static_cast<u32>(b))) {
            refuse(ctx, std::format("_sys_memcmp({:#x}, {:#x}, {:#x}): not guest memory; 0", buf1, buf2, size));
            return 0;
        }
        const u64 n = std::min({u64{size} - i, (a & ~(page - 1)) + page - a, (b & ~(page - 1)) + page - b});
        if (const int r = std::memcmp(base + a, base + b, n)) return r < 0 ? -1 : 1;
        i += n;
    }
    return 0;
}

// sys_libc's (RPCS3 sys_libc_*): the destination back; memcmp's result zero-extended, as RPCS3's u32.
u32 sys_libc_memcpy(PPUContext& ctx, u32 dst, u32 src, u32 size) { return copy_memory(ctx, "memcpy", dst, src, size); }
u32 sys_libc_memmove(PPUContext& ctx, u32 dst, u32 src, u32 size) { return copy_memory(ctx, "memmove", dst, src, size); }
u32 sys_libc_memset(PPUContext& ctx, u32 dst, s32 value, u32 size) { return _sys_memset(ctx, dst, value, size); }
u32 sys_libc_memcmp(PPUContext& ctx, u32 a, u32 b, u32 size) { return static_cast<u32>(_sys_memcmp(ctx, a, b, size)); }

// RPCS3: NULL for a null buffer or a size that is not positive.
u32 _sys_memchr(PPUContext& ctx, u32 buf, u8 ch, s32 size)
{
    if (!buf || size <= 0) return 0;
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u64 page = host_page();
    const u64 end = u64{buf} + static_cast<u32>(size);
    for (u64 a = buf; a < end;) {
        if (a >= GuestMemory::kGuestSize || !t.kernel.memory().is_committed(static_cast<u32>(a))) {
            refuse(ctx, std::format("_sys_memchr({:#x}, {:#x}, {:#x}): not guest memory; NULL", buf, ch, size));
            return 0;
        }
        const u64 stop = std::min(end, (a & ~(page - 1)) + page);
        if (const void* p = std::memchr(base + a, ch, stop - a)) {
            return static_cast<u32>(static_cast<const u8*>(p) - base);
        }
        a = stop;
    }
    return 0;
}

// ------------------------------------------------------------------------------------------------- strings

// RPCS3: 0 for a null string.
u32 _sys_strlen(PPUContext& ctx, u32 str)
{
    if (!str) return 0;
    const std::optional<u64> n = guest_strnlen(thread_of(ctx), str, kNoLimit);
    if (!n) {
        refuse(ctx, std::format("_sys_strlen({:#x}): not guest memory; 0", str));
        return 0;
    }
    return static_cast<u32>(*n);
}

// RPCS3 _sys_strcmp / _sys_strncmp / _sys_strncasecmp: bytes compared unsigned (case folded for strncasecmp), at most
// `max` of them, up to a NUL in both; -1, 0 or 1. nullopt when memory that is not committed comes first.
std::optional<s32> compare_strings(lv2::PpuThread& t, u32 str1, u32 str2, u64 max, bool fold_case)
{
    GuestBytes a(t, str1), b(t, str2);
    for (u64 i = 0; i < max; i++) {
        int c1 = a.at(i), c2 = b.at(i);
        if (c1 < 0 || c2 < 0) return std::nullopt;
        if (fold_case) {
            c1 = fold(c1);
            c2 = fold(c2);
        }
        if (c1 != c2) return c1 < c2 ? -1 : 1;
        if (c1 == 0) break;
    }
    return 0;
}

s32 _sys_strcmp(PPUContext& ctx, u32 str1, u32 str2)
{
    const std::optional<s32> r = compare_strings(thread_of(ctx), str1, str2, kNoLimit, false);
    if (!r) refuse(ctx, std::format("_sys_strcmp({:#x}, {:#x}): not guest memory; 0", str1, str2));
    return r.value_or(0);
}

s32 _sys_strncmp(PPUContext& ctx, u32 str1, u32 str2, u32 max)
{
    const std::optional<s32> r = compare_strings(thread_of(ctx), str1, str2, max, false);
    if (!r) refuse(ctx, std::format("_sys_strncmp({:#x}, {:#x}, {}): not guest memory; 0", str1, str2, max));
    return r.value_or(0);
}

// RPCS3 folds through _sys_tolower, which aborts on a byte past 127 (a negative host char); such bytes are compared as
// they are here.
s32 _sys_strncasecmp(PPUContext& ctx, u32 str1, u32 str2, u32 n)
{
    const std::optional<s32> r = compare_strings(thread_of(ctx), str1, str2, n, true);
    if (!r) refuse(ctx, std::format("_sys_strncasecmp({:#x}, {:#x}, {}): not guest memory; 0", str1, str2, n));
    return r.value_or(0);
}

// Copies `n` bytes of the guest string at `src` to `dst` and puts a NUL after them, when both are guest memory.
bool put_string(lv2::PpuThread& t, u32 dst, u32 src, u64 n)
{
    if (!guest_range(t, dst, n + 1) || !guest_range(t, src, n)) return false;
    u8* base = t.kernel.base();
    std::memmove(base + dst, base + src, n);
    base[dst + n] = 0;
    return true;
}

u32 _sys_strcpy(PPUContext& ctx, u32 dst, u32 src)
{
    lv2::PpuThread& t = thread_of(ctx);
    const std::optional<u64> n = guest_strnlen(t, src, kNoLimit);
    if (!n || !put_string(t, dst, src, *n))
        refuse(ctx, std::format("_sys_strcpy({:#x}, {:#x}): not guest memory; nothing copied", dst, src));
    return dst;
}

// RPCS3: NULL when either pointer is; copies up to `len` bytes, the rest of them NULs once the string has ended.
u32 _sys_strncpy(PPUContext& ctx, u32 dst, u32 src, s32 len)
{
    if (!dst || !src) return 0;
    if (len <= 0) return dst;
    lv2::PpuThread& t = thread_of(ctx);
    const std::optional<u64> n = guest_strnlen(t, src, static_cast<u32>(len));
    if (!n || !guest_range(t, dst, static_cast<u32>(len))) {
        refuse(ctx, std::format("_sys_strncpy({:#x}, {:#x}, {}): not guest memory; nothing copied", dst, src, len));
        return dst;
    }
    u8* base = t.kernel.base();
    std::memmove(base + dst, base + src, *n);
    std::memset(base + dst + *n, 0, static_cast<u32>(len) - *n);
    return dst;
}

u32 _sys_strcat(PPUContext& ctx, u32 dst, u32 src)
{
    lv2::PpuThread& t = thread_of(ctx);
    const std::optional<u64> end = guest_strnlen(t, dst, kNoLimit), n = guest_strnlen(t, src, kNoLimit);
    if (!end || !n || !put_string(t, static_cast<u32>(dst + *end), src, *n))
        refuse(ctx, std::format("_sys_strcat({:#x}, {:#x}): not guest memory; nothing appended", dst, src));
    return dst;
}

// RPCS3: up to `max` bytes of `src` after the end of `dst`, then a NUL.
u32 _sys_strncat(PPUContext& ctx, u32 dst, u32 src, u32 max)
{
    lv2::PpuThread& t = thread_of(ctx);
    const std::optional<u64> end = guest_strnlen(t, dst, kNoLimit), n = guest_strnlen(t, src, max);
    if (!end || !n || !put_string(t, static_cast<u32>(dst + *end), src, *n))
        refuse(ctx, std::format("_sys_strncat({:#x}, {:#x}, {}): not guest memory; nothing appended", dst, src, max));
    return dst;
}

// RPCS3: the first `ch` (the NUL itself for 0), or NULL.
u32 _sys_strchr(PPUContext& ctx, u32 str, u8 ch)
{
    GuestBytes s(thread_of(ctx), str);
    for (u64 i = 0;; i++) {
        const int c = s.at(i);
        if (c < 0) {
            refuse(ctx, std::format("_sys_strchr({:#x}, {:#x}): not guest memory; NULL", str, ch));
            return 0;
        }
        if (c == ch) return static_cast<u32>(str + i);
        if (c == 0) return 0;
    }
}

// RPCS3: the last `ch` (the NUL itself for 0), or NULL.
u32 _sys_strrchr(PPUContext& ctx, u32 str, u8 ch)
{
    GuestBytes s(thread_of(ctx), str);
    u32 last = 0;
    for (u64 i = 0;; i++) {
        const int c = s.at(i);
        if (c < 0) {
            refuse(ctx, std::format("_sys_strrchr({:#x}, {:#x}): not guest memory; NULL", str, ch));
            return 0;
        }
        if (c == ch) last = static_cast<u32>(str + i);
        if (c == 0) return last;
    }
}

// ------------------------------------------------------------------------------------------------- printf

// The variadic arguments of a call after its `fixed` named ones, as the PPC64 ELF ABI passes them (RPCS3 ps3_fmt_src):
// one 64-bit slot each, argument n in r(3 + n) for n < 8, else in the caller's parameter save area at r1 + 0x30 + 8n.
// A double is in its slot as well (a variadic call puts it there besides an FPR), which is where RPCS3 reads it too.
// Deviation: RPCS3 reads the ninth argument and the ones after it one slot low (it passes its 0-based index to
// get_stack_arg, which counts from 1); here from where the caller put them.
// Or the arguments of a va_list (VarArgs::list): the ABI's va_list is the address of the next argument's 64-bit slot,
// va_start having stored r4..r10 into the parameter save area beside the ones the caller put there (liblv2's variadic
// functions start with those stores and hand their formatter r1 + the first slot), so argument n is at ap + 8n.
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
    bool unreadable() const { return unreadable_; }

private:
    PPUContext& ctx_;
    const u32 sp_; // r1 at the call (StackVars move it later)
    u32 next_;
    u32 list_ = 0;
    bool from_list_ = false;
    bool unreadable_ = false;
};

// A conversion's flags, width and precision.
struct Conversion {
    bool left = false, sign = false, space = false, alter = false, zeros = false, dot = false;
    u32 width = 0, prec = 0;
};

// Widths and precisions are cut to this, so a "%999999999d" cannot exhaust the host's memory.
constexpr u32 kMaxWidth = 0x10000;

// `v` in `radix`, at least `prec` digits with a precision, else at least one; none for 0 at precision 0 (C99).
std::string digits(u64 v, u32 radix, bool upper, const Conversion& c)
{
    const char* set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string d;
    for (; v; v /= radix) d += set[v % radix];
    const u32 want = c.dot ? c.prec : 1;
    if (d.size() < want) d.append(want - d.size(), '0');
    std::reverse(d.begin(), d.end());
    return d;
}

// `prefix` + `body` padded to the width: spaces before them, or after them with '-', or zeros between them with '0'
// where `zero_fill` allows it.
void put_padded(std::string& out, const Conversion& c, std::string_view prefix, std::string_view body, bool zero_fill)
{
    const usize n = prefix.size() + body.size();
    const usize fill = c.width > n ? c.width - n : 0;
    if (c.left) {
        out += prefix;
        out += body;
        out.append(fill, ' ');
    } else if (zero_fill && c.zeros) {
        out += prefix;
        out.append(fill, '0');
        out += body;
    } else {
        out.append(fill, ' ');
        out += prefix;
        out += body;
    }
}

// liblv2's formatter, which _sys_printf, _sys_sprintf, _sys_snprintf and their v forms share (the function at 0xc958
// of liblv2.sprx 4.92), compared call for call with made-up formats in tests/hle/test_lv2_compare.cpp (D-501). It is a
// small printf of integers, not RPCS3's ps3_fmt (Utilities/cfmt.h, which this followed before):
// - '%', then flags (- + space # 0), a width (digits, or '*' whose negative value means '-'), a precision ('.' and
//   digits, or ".*" whose negative value means none) and a length (h and hh, which change nothing but %n's store; l,
//   32 bits as int; a second l, or L, 64) in any order and any number of times, each later one replacing what it
//   sets, then the conversion (liblv2 looks every character after '%' up in one table).
// - d i u o x X c s p n %, as C99 has them, and D O U as d o u, except: %p is "0x" and the hex digits (a precision
//   ignored, '0' filling after the 0x); '0' fills %s and %c with zeros too; '#' gives an octal number a leading 0
//   only when it has digits (so "%#.0o" of 0 is empty); %n prints nothing and stores the count.
// - Any other character, the floating conversions f e g a among them, is printed as it is and takes no argument; a
//   sequence the format ends in prints nothing.
// - %s of a null pointer prints "(null)"; a %s or stack argument that is not guest memory is noted in `problems` and
//   printed as nothing or 0 (liblv2 faults).
std::string format_args(lv2::PpuThread& t, std::string_view fmt, VarArgs& args, std::vector<std::string>& problems)
{
    std::string out;
    usize i = 0;
    const auto at = [&](usize k) { return k < fmt.size() ? fmt[k] : '\0'; };
    const auto decimal = [&] { // digits as liblv2 adds them up, wrapping at 32 bits
        u32 r = 0;
        while (at(i) >= '0' && at(i) <= '9') r = r * 10 + static_cast<u32>(fmt[i++] - '0');
        return r;
    };
    while (i < fmt.size()) {
        const char first = fmt[i++];
        if (first != '%') {
            out += first;
            continue;
        }
        // After '%' every character goes through one table, as in liblv2: flags, width digits, '*', '.' and the
        // length letters change the conversion's state and the next character is looked at, in any order and as often
        // as they come (a later width replaces an earlier one: "%5*d" takes its width from the argument); the first
        // other character is the conversion.
        Conversion c;
        u32 size = 4;
        bool half = false;      // h or hh: only %n's store is shorter for it
        bool long_once = false; // one l: 32 bits still; a second one makes it 64
        char conv = 0;
        while (i < fmt.size() && !conv) {
            const char ch = fmt[i++];
            switch (ch) {
            case '-': c.left = true; break;
            case '+': c.sign = true; break;
            case ' ': c.space = true; break; // printed only without '+'
            case '#': c.alter = true; break;
            case '0': c.zeros = true; break;
            case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': {
                i--;
                const u32 w = decimal(); // wraps at 32 bits as liblv2's does; a negative width pads nothing
                c.width = static_cast<s32>(w) < 0 ? 0 : std::min(w, kMaxWidth);
                break;
            }
            case '*': {
                const s32 w = static_cast<s32>(args.next());
                c.width = std::min<u32>(w < 0 ? 0u - static_cast<u32>(w) : static_cast<u32>(w), kMaxWidth);
                c.left |= w < 0;
                break;
            }
            case '.':
                if (at(i) == '*') {
                    i++;
                    const s32 p = static_cast<s32>(args.next());
                    c.dot = p >= 0; // a negative one: none
                    c.prec = c.dot ? std::min<u32>(static_cast<u32>(p), kMaxWidth) : 0;
                } else {
                    const u32 p = decimal(); // none digits: 0; one past 31 bits: none
                    c.dot = static_cast<s32>(p) >= 0;
                    c.prec = c.dot ? std::min(p, kMaxWidth) : 0;
                }
                break;
            case 'h': half = true; break;
            case 'l':
                if (long_once) size = 8;
                long_once = !long_once;
                break;
            case 'L': size = 8; break;
            case 'D': case 'O': case 'U': conv = static_cast<char>(ch + ('a' - 'A')); break; // as d o u
            default: conv = ch; break;
            }
        }
        if (!conv) break; // a sequence the format ends in prints nothing
        switch (conv) {
        case 'd':
        case 'i': {
            const u64 raw = args.next();
            const s64 v = size == 8 ? static_cast<s64>(raw) : s64{static_cast<s32>(raw)};
            const u64 magnitude = v < 0 ? 0 - static_cast<u64>(v) : static_cast<u64>(v);
            put_padded(out, c, v < 0 ? "-" : c.sign ? "+" : c.space ? " " : "", digits(magnitude, 10, false, c),
                !c.dot);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            const u64 raw = args.next();
            const u64 v = size == 8 ? raw : static_cast<u32>(raw);
            std::string_view prefix;
            std::string body = digits(v, conv == 'u' ? 10 : conv == 'o' ? 8 : 16, conv == 'X', c);
            if (conv == 'o' && c.alter && !body.empty() && body[0] != '0') body.insert(body.begin(), '0');
            if ((conv == 'x' || conv == 'X') && c.alter && v) prefix = conv == 'x' ? "0x" : "0X";
            put_padded(out, c, prefix, body, !c.dot);
            break;
        }
        case 'c': {
            const char v = static_cast<char>(args.next() & 0xFF);
            put_padded(out, c, "", std::string_view(&v, 1), true);
            break;
        }
        case 's': {
            const u32 addr = static_cast<u32>(args.next());
            std::string s;
            if (!addr) {
                s = "(null)";
            } else if (const std::optional<u64> n = guest_strnlen(t, addr, c.dot ? c.prec : kMaxString)) {
                s.assign(reinterpret_cast<const char*>(t.kernel.base() + addr), *n);
            } else {
                problems.push_back(std::format("the %s argument {:#x} is not guest memory (printed as nothing)", addr));
            }
            if (c.dot && s.size() > c.prec) s.resize(c.prec);
            put_padded(out, c, "", s, true);
            break;
        }
        case 'p': {
            Conversion hex = c;
            hex.dot = false;
            put_padded(out, c, "0x", digits(static_cast<u32>(args.next()), 16, false, hex), true);
            break;
        }
        case 'n': { // the number of characters so far, stored at the argument: 4 bytes (ll too), 2 with h or hh
            const u32 addr = static_cast<u32>(args.next());
            const u32 bytes = half ? 2 : 4;
            if (!guest_range(t, addr, bytes)) {
                problems.push_back(std::format("the %n argument {:#x} is not guest memory (nothing stored)", addr));
            } else if (bytes == 2) {
                ps_write16(t.kernel.base(), addr, static_cast<u16>(out.size()));
            } else {
                ps_write32(t.kernel.base(), addr, static_cast<u32>(out.size()));
            }
            break;
        }
        default: // '%' and any character liblv2 does not convert
            out += conv;
            break;
        }
    }
    return out;
}

// The text of a printf-family call: the guest format at `fmt` with `args`. False (the call site logged) when the format
// is not guest memory: RPCS3 faults, the call returns CELL_EFAULT here.
bool format_text(PPUContext& ctx, std::string_view what, u32 fmt, VarArgs& args, std::string& out)
{
    lv2::PpuThread& t = thread_of(ctx);
    const std::optional<u64> n = fmt ? guest_strnlen(t, fmt, kMaxString) : std::nullopt;
    if (!n) {
        refuse(ctx, std::format("{}: the format {:#x} is not guest memory; CELL_EFAULT", what, fmt));
        return false;
    }
    const std::string format(reinterpret_cast<const char*>(t.kernel.base() + fmt), *n);
    std::vector<std::string> problems;
    out = format_args(t, format, args, problems);
    if (args.unreadable()) problems.emplace_back("an argument on the stack is not guest memory (read as 0)");
    if (!problems.empty()) {
        std::string why = std::format("{}(\"{}\"):", what, format);
        for (const std::string& p : problems) why += " " + p + ";";
        refuse(ctx, why);
    }
    PS_LOG_TRACE(kChan, "{}(\"{}\") -> \"{}\"", what, format, out);
    return true;
}

// ... with the arguments after the call's `fixed` named ones.
bool format_call(PPUContext& ctx, std::string_view what, u32 fmt, u32 fixed, std::string& out)
{
    VarArgs args(ctx, fixed);
    return format_text(ctx, what, fmt, args, out);
}

// ... with the arguments of the va_list `ap` (the v functions).
bool format_list(PPUContext& ctx, std::string_view what, u32 fmt, u32 ap, std::string& out)
{
    VarArgs args = VarArgs::list(ctx, ap);
    return format_text(ctx, what, fmt, args, out);
}

// The text to sys_tty_write on channel 0, from guest memory (RPCS3 _sys_printf; liblv2 writes it 127 bytes at a time).
// Here in pieces of up to 1 KB on the caller's stack, each ending after a line where one ends in it, so a long text
// needs no more stack than that.
void tty_print(PPUContext& ctx, std::string_view text)
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
        StackVar piece(ctx, base, kPiece + 8); // the text, then sys_tty_write's written length
        std::memcpy(base + piece.addr(), rest.data(), n);
        lv2::sys_tty_write(t, 0, piece.addr(), static_cast<u32>(n), piece.addr() + kPiece);
        rest.remove_prefix(n);
    }
}

// liblv2's: the number of characters printed (RPCS3 returns CELL_OK).
s32 _sys_printf(PPUContext& ctx, u32 fmt)
{
    std::string text;
    if (!format_call(ctx, "_sys_printf", fmt, 1, text)) return err(lv2::CELL_EFAULT);
    tty_print(ctx, text);
    return static_cast<s32>(text.size());
}

// liblv2's _sys_vprintf: _sys_printf with a va_list.
s32 _sys_vprintf(PPUContext& ctx, u32 fmt, u32 ap)
{
    std::string text;
    if (!format_list(ctx, "_sys_vprintf", fmt, ap, text)) return err(lv2::CELL_EFAULT);
    tty_print(ctx, text);
    return static_cast<s32>(text.size());
}

// RPCS3: the text and its NUL into `buffer`; its length.
s32 put_sprintf(PPUContext& ctx, std::string_view what, u32 buffer, std::string_view text)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_range(t, buffer, text.size() + 1)) {
        refuse(ctx, std::format("{}({:#x}): {} bytes do not fit guest memory there; CELL_EFAULT", what, buffer,
            text.size() + 1));
        return err(lv2::CELL_EFAULT);
    }
    std::memcpy(t.kernel.base() + buffer, text.data(), text.size());
    t.kernel.base()[buffer + text.size()] = 0;
    return static_cast<s32>(text.size());
}

s32 _sys_sprintf(PPUContext& ctx, u32 buffer, u32 fmt)
{
    std::string text;
    if (!format_call(ctx, "_sys_sprintf", fmt, 2, text)) return err(lv2::CELL_EFAULT);
    return put_sprintf(ctx, "_sys_sprintf", buffer, text);
}

// liblv2's _sys_vsprintf: _sys_sprintf with a va_list.
s32 _sys_vsprintf(PPUContext& ctx, u32 buffer, u32 fmt, u32 ap)
{
    std::string text;
    if (!format_list(ctx, "_sys_vsprintf", fmt, ap, text)) return err(lv2::CELL_EFAULT);
    return put_sprintf(ctx, "_sys_vsprintf", buffer, text);
}

// liblv2's (compared in tests/hle/test_lv2_compare.cpp, D-501): up to count - 1 bytes of the text, the text's whole
// length back (as C99's snprintf; RPCS3 returns the bytes written), and, for a count other than 0, a NUL at dst + that
// length: past the bytes written, and past the buffer when the text was cut, which is what liblv2 does.
s32 put_snprintf(PPUContext& ctx, std::string_view what, u32 dst, u32 count, std::string_view text)
{
    if (!count) return static_cast<s32>(text.size());
    const u32 n = static_cast<u32>(std::min<usize>(count - 1, text.size()));
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_range(t, dst, n) || !guest_range(t, static_cast<u32>(dst + text.size()), 1)) {
        refuse(ctx, std::format("{}({:#x}, {}): not guest memory; CELL_EFAULT", what, dst, count));
        return err(lv2::CELL_EFAULT);
    }
    std::memcpy(t.kernel.base() + dst, text.data(), n);
    t.kernel.base()[dst + text.size()] = 0;
    return static_cast<s32>(text.size());
}

s32 _sys_snprintf(PPUContext& ctx, u32 dst, u32 count, u32 fmt)
{
    std::string text;
    if (!format_call(ctx, "_sys_snprintf", fmt, 3, text)) return err(lv2::CELL_EFAULT);
    return put_snprintf(ctx, "_sys_snprintf", dst, count, text);
}

// liblv2's _sys_vsnprintf: _sys_snprintf with a va_list.
s32 _sys_vsnprintf(PPUContext& ctx, u32 dst, u32 count, u32 fmt, u32 ap)
{
    std::string text;
    if (!format_list(ctx, "_sys_vsnprintf", fmt, ap, text)) return err(lv2::CELL_EFAULT);
    return put_snprintf(ctx, "_sys_vsnprintf", dst, count, text);
}

// liblv2's _sys_panic (code read from the firmware's module, D-500): the message as _sys_printf prints it, then the
// process ends with CELL_EABORT through the syscall itself (no atexit handlers run). liblv2 also tells a debugger
// first (syscall 988, which RPCS3 and pspocket leave unanswered) and traps instead when one is attached.
[[noreturn]] void panic_exit(PPUContext& ctx, std::string_view text)
{
    tty_print(ctx, text);
    PS_LOG_ERROR(kChan, "_sys_panic: \"{}\": the process ends (CELL_EABORT)", text);
    lv2::_sys_process_exit(thread_of(ctx), static_cast<s32>(lv2::CELL_EABORT), 0, 0);
    PS_FATAL(kChan, "_sys_process_exit returned");
}

void _sys_panic(PPUContext& ctx, u32 fmt)
{
    std::string text;
    if (!format_call(ctx, "_sys_panic", fmt, 1, text)) text = "(the panic message is not guest memory)\n";
    panic_exit(ctx, text);
}

// liblv2's export 0x9FB6228E, whose name is not known (libfiber, libspurs_jq and libsync2 import it for their fatal
// errors): the message by _sys_vprintf, then _sys_panic("\n").
void sysPrxForUser_9FB6228E(PPUContext& ctx, u32 fmt)
{
    std::string text;
    if (!format_call(ctx, "sysPrxForUser_9FB6228E", fmt, 1, text)) text.clear();
    tty_print(ctx, text);
    panic_exit(ctx, "\n");
}

// liblv2's __tls_get_addr: not supported there; it panics with this message.
u32 __tls_get_addr(PPUContext& ctx)
{
    panic_exit(ctx, "__tls_get_addr called -> panic\n");
}

// liblv2's __sys_printf_basename: the part of the path after its last '/' that something follows (the whole path
// without one; "dir/" stays "dir/", as liblv2's loop keeps the start when a '/' ends the path).
u32 __sys_printf_basename(PPUContext& ctx, u32 path)
{
    GuestBytes s(thread_of(ctx), path);
    u32 base = path;
    for (u64 i = 0;; i++) {
        const int c = s.at(i);
        const int next = c == '/' ? s.at(i + 1) : 1;
        if (c < 0 || next < 0) {
            refuse(ctx, std::format("__sys_printf_basename({:#x}): not guest memory; the path itself", path));
            return path;
        }
        if (c == 0 || next == 0) return base;
        if (c == '/') base = static_cast<u32>(path + i + 1);
    }
}

// ------------------------------------------------------------------------------------------------- heaps, malloc

// liblv2's heaps (D-502), read from liblv2.sprx and compared with it (tests/hle/test_lv2_compare.cpp). A heap is a
// dlmalloc mspace (lv2_heap.hpp) in memory of its own, and its ID is the address of its malloc_state:
// - _sys_heap_create_heap(name, size, flags, attr) takes the memory from sys_memory_allocate (size rounded up to 64 KB,
//   64 KB pages), or, with the flags' top bit set and an attribute {u32 8, u32 address} whose address is not 0 (and
//   flag 1 clear), the caller's memory at that address, `size` bytes of it. The name (31 characters) and the flags go
//   into the malloc_state where liblv2 keeps them; 0 when the attribute is not 8 bytes or the memory cannot be had.
// - _sys_heap_delete_heap gives the memory back with sys_memory_free when liblv2 took it (the flags' top bit clear).
// - _sys_malloc, _sys_memalign and _sys_free use liblv2's own heap, "__sys_heap": 512 KB, flags 0x110000, made as
//   liblv2 starts (here at the first of them), so liblv2's C library has 512 KB in all, as on a PS3.
// - A heap of 0 prints "invalid heap area 0x0" (and _sys_heap_free and _sys_heap_stats return what printf did).
// liblv2 holds each heap's lwmutex around its calls; one host mutex for all of them here (no guest code runs inside).
// Before, as RPCS3's sys_heap.cpp: an ID only, and every block a 64 KB-aligned allocation of the main area.
struct HeapState {
    std::mutex mutex;
    lv2heap::Params params;
    bool params_set = false;
    u32 count = 0;              // liblv2 counts its heaps
    std::atomic<u32> own{0};    // "__sys_heap"
    std::atomic<bool> own_tried{false};
};

HeapState& heaps(PPUContext&)
{
    HeapState& s = HleProcess::current()->state<HeapState>();
    return s;
}

// liblv2's init_mparams: the magic from the timebase's low word, as dlmalloc makes it from time(0).
const lv2heap::Params& heap_params(HeapState& s)
{
    if (!s.params_set) {
        const u32 t = static_cast<u32>(ps_ppu_timebase());
        s.params.magic = ((t ^ 0x55555555u) | 8u) & ~7u;
        s.params_set = true;
    }
    return s.params;
}

s32 invalid_heap(PPUContext& ctx, u32 heap)
{
    const std::string text = std::format("invalid heap area 0x{:x}\n", heap);
    tty_print(ctx, text);
    return static_cast<s32>(text.size());
}

u32 _sys_heap_create_heap(PPUContext& ctx, u32 name, u32 size, u32 flags, u32 attr)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    u32 memory = 0, capacity = 0, info_flags = 0;
    bool own_memory = true;
    if (attr) {
        if (!guest_range(t, attr, 8) || ps_read32(base, attr) != 8) return 0;
        if ((flags & 0x80000000u) && ps_read32(base, attr + 4) != 0 && !(flags & 1)) {
            own_memory = false;
            memory = ps_read32(base, attr + 4);
            capacity = size;
            info_flags = flags;
        }
    }
    if (own_memory) {
        capacity = static_cast<u32>((u64{size} + 0xFFFF) & ~u64{0xFFFF});
        StackVar addr(ctx, base, 8);
        if (lv2::sys_memory_allocate(t, capacity, 0x200, addr.addr()) != lv2::CELL_OK) return 0;
        memory = ps_read32(base, addr.addr());
        info_flags = flags & 0x7FFFFFFF;
    }
    HeapState& s = heaps(ctx);
    std::lock_guard lock(s.mutex);
    lv2heap::Space space(base, heap_params(s));
    const u32 m = guest_range(t, memory, capacity) ? space.create_with_base(memory, capacity) : 0;
    if (m) {
        ps_write32(base, m + lv2heap::kStateFlags, info_flags);
        const std::string n = name ? read_cstr(t, name, lv2heap::kStateNameMax).text : std::string();
        for (u32 i = 0; i < lv2heap::kStateNameMax; i++)
            ps_write8(base, m + lv2heap::kStateName + i, i < n.size() ? static_cast<u8>(n[i]) : 0);
        ps_write8(base, m + lv2heap::kStateName + lv2heap::kStateNameMax, 0);
    }
    s.count++;
    PS_LOG_DEBUG(kChan, "_sys_heap_create_heap({:#x}, {:#x}, {:#x}, {:#x}) -> {:#x}", name, size, flags, attr, m);
    return m;
}

s32 _sys_heap_delete_heap(PPUContext& ctx, u32 heap)
{
    if (!heap) {
        invalid_heap(ctx, heap);
        return lv2::CELL_OK;
    }
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    HeapState& s = heaps(ctx);
    std::lock_guard lock(s.mutex);
    if (!guest_range(t, heap, lv2heap::kStateSize)) {
        refuse(ctx, std::format("_sys_heap_delete_heap({:#x}): not guest memory (liblv2 faults)", heap));
        return lv2::CELL_OK;
    }
    const u32 info_flags = ps_read32(base, heap + lv2heap::kStateFlags);
    const u32 memory = ps_read32(base, heap + 16); // least_addr: the segment's base
    lv2heap::Space(base, heap_params(s)).destroy(heap);
    if (!(info_flags & 0x80000000u)) lv2::sys_memory_free(t, memory);
    s.count--;
    return lv2::CELL_OK;
}

// A heap's operation under the lock, when the heap is not 0 and its malloc_state is guest memory.
template <class F>
auto with_heap(PPUContext& ctx, u32 heap, F&& f)
{
    lv2::PpuThread& t = thread_of(ctx);
    HeapState& s = heaps(ctx);
    std::lock_guard lock(s.mutex);
    lv2heap::Space space(t.kernel.base(), heap_params(s));
    return f(space, guest_range(t, heap, lv2heap::kStateSize));
}

u32 _sys_heap_malloc(PPUContext& ctx, u32 heap, u32 size)
{
    if (!heap) {
        invalid_heap(ctx, heap);
        return 0;
    }
    return with_heap(ctx, heap, [&](lv2heap::Space& space, bool ok) { return ok ? space.malloc(heap, size) : 0u; });
}

u32 _sys_heap_memalign(PPUContext& ctx, u32 heap, u32 align, u32 size)
{
    if (!heap) {
        invalid_heap(ctx, heap);
        return 0;
    }
    return with_heap(ctx, heap,
        [&](lv2heap::Space& space, bool ok) { return ok ? space.memalign(heap, align, size) : 0u; });
}

s32 _sys_heap_free(PPUContext& ctx, u32 heap, u32 addr)
{
    if (!heap) return invalid_heap(ctx, heap);
    with_heap(ctx, heap, [&](lv2heap::Space& space, bool ok) {
        if (ok && (!addr || guest_range(thread_of(ctx), addr - 8, 8))) space.free(heap, addr);
        return 0;
    });
    return lv2::CELL_OK;
}

// liblv2: malloc, or with an attribute {u32 size > 7, u32 alignment} memalign by a power of two that is a multiple of
// 4 (0 for any other).
u32 _sys_heap_alloc_heap_memory(PPUContext& ctx, u32 heap, u32 size, u32 attr)
{
    if (!heap) {
        invalid_heap(ctx, heap);
        return 0;
    }
    lv2::PpuThread& t = thread_of(ctx);
    u32 align = 0;
    if (attr) {
        if (!guest_range(t, attr, 8) || ps_read32(t.kernel.base(), attr) <= 7) return 0;
        align = ps_read32(t.kernel.base(), attr + 4);
        if (!align || (align & 3) || !std::has_single_bit(align)) return 0;
    }
    return with_heap(ctx, heap, [&](lv2heap::Space& space, bool ok) {
        return !ok ? 0u : align ? space.memalign(heap, align, size) : space.malloc(heap, size);
    });
}

// struct mallinfo's ten words into `info`.
s32 _sys_heap_get_mallinfo(PPUContext& ctx, u32 heap, u32 info)
{
    if (!heap) {
        invalid_heap(ctx, heap);
        return lv2::CELL_OK;
    }
    lv2::PpuThread& t = thread_of(ctx);
    const lv2heap::MallInfo mi =
        with_heap(ctx, heap, [&](lv2heap::Space& space, bool ok) { return ok ? space.mallinfo(heap) : lv2heap::MallInfo{}; });
    if (!guest_range(t, info, 40)) {
        refuse(ctx, std::format("_sys_heap_get_mallinfo({:#x}, {:#x}): not guest memory (liblv2 faults)", heap, info));
        return lv2::CELL_OK;
    }
    const u32 words[] = {mi.arena, mi.ordblks, mi.smblks, mi.hblks, mi.hblkhd, mi.usmblks, mi.fsmblks, mi.uordblks,
                         mi.fordblks, mi.keepcost};
    for (u32 i = 0; i < 10; i++) ps_write32(t.kernel.base(), info + 4 * i, words[i]);
    return lv2::CELL_OK;
}

// mallinfo's fordblks: the free bytes, the top's included.
u32 _sys_heap_get_total_free_size(PPUContext& ctx, u32 heap)
{
    if (!heap) {
        invalid_heap(ctx, heap);
        return 0;
    }
    return with_heap(ctx, heap, [&](lv2heap::Space& space, bool ok) { return ok ? space.mallinfo(heap).fordblks : 0u; });
}

// dlmalloc's malloc_stats, whose printing liblv2 leaves out: 0 for a heap, the heap itself when it is not one.
u32 _sys_heap_stats(PPUContext& ctx, u32 heap)
{
    if (!heap) return static_cast<u32>(invalid_heap(ctx, heap));
    return with_heap(ctx, heap, [&](lv2heap::Space& space, bool ok) {
        if (!ok || !space.ok_magic(heap)) return heap;
        space.malloc_stats(heap);
        return 0u;
    });
}

// liblv2's own heap, made with the first call that needs it (liblv2 makes it as it starts); a process that cannot have
// its 512 KB ends there with CELL_EAGAIN.
u32 own_heap(PPUContext& ctx)
{
    HeapState& s = heaps(ctx);
    if (const u32 h = s.own.load(std::memory_order_acquire)) return h;
    static std::mutex make;
    std::lock_guard lock(make);
    if (const u32 h = s.own.load(std::memory_order_acquire)) return h;
    lv2::PpuThread& t = thread_of(ctx);
    StackVar name(ctx, t.kernel.base(), 16);
    std::memcpy(t.kernel.base() + name.addr(), "__sys_heap", 11);
    const u32 h = _sys_heap_create_heap(ctx, name.addr(), 0x80000, 0x110000, 0);
    if (!h) {
        PS_LOG_ERROR(kChan, "liblv2's heap (512 KB) could not be made: the process ends (CELL_EAGAIN), as liblv2's does");
        lv2::_sys_process_exit(t, static_cast<s32>(lv2::CELL_EAGAIN), 0, 0);
    }
    s.own.store(h, std::memory_order_release);
    return h;
}

u32 _sys_malloc(PPUContext& ctx, u32 size) { return _sys_heap_malloc(ctx, own_heap(ctx), size); }
u32 _sys_memalign(PPUContext& ctx, u32 align, u32 size) { return _sys_heap_memalign(ctx, own_heap(ctx), align, size); }
void _sys_free(PPUContext& ctx, u32 addr) { _sys_heap_free(ctx, own_heap(ctx), addr); }

// ------------------------------------------------------------------------------------------------- qsort

// RPCS3 _sys_qsort: the host's std::qsort on the array in place, comparing through the guest function `cmp` (a
// descriptor) on the calling thread. Here a merge sort of the elements' indices: the comparator gets the guest
// addresses of the elements where they lie, as qsort may give them, and the array is written in its new order at the
// end. It makes about n log2 n comparisons and is stable (qsort need not be); a comparator that is not consistent
// cannot make it go outside the array, as it can a host qsort. An array or a comparator that is not guest memory is
// refused with the call site logged.
void _sys_qsort(PPUContext& ctx, u32 base_addr, u32 nelem, u32 size, u32 cmp)
{
    PS_LOG_DEBUG(kChan, "_sys_qsort({:#x}, {}, {:#x}, {:#x})", base_addr, nelem, size, cmp);
    if (nelem < 2 || size == 0) return;
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_range(t, base_addr, u64{nelem} * size) || !guest_readable(t, cmp, 8)) {
        refuse(ctx, std::format("_sys_qsort({:#x}, {}, {:#x}, {:#x}): the array or the comparator is not guest memory; "
                                "not sorted", base_addr, nelem, size, cmp));
        return;
    }
    const auto element = [&](u32 index) { return u64{base_addr} + u64{index} * size; };
    std::vector<u32> order(nelem), merged(nelem);
    for (u32 k = 0; k < nelem; k++) order[k] = k;
    for (u64 width = 1; width < nelem; width *= 2) {
        for (u64 lo = 0; lo < nelem; lo += 2 * width) {
            const u64 mid = std::min<u64>(lo + width, nelem), hi = std::min<u64>(lo + 2 * width, nelem);
            u64 a = lo, b = mid, k = lo;
            while (a < mid && b < hi) {
                const s32 r = static_cast<s32>(call_guest(ctx, cmp, {element(order[a]), element(order[b])}));
                merged[k++] = r > 0 ? order[b++] : order[a++];
            }
            while (a < mid) merged[k++] = order[a++];
            while (b < hi) merged[k++] = order[b++];
        }
        order.swap(merged);
    }
    u8* base = t.kernel.base();
    const std::vector<u8> copy(base + base_addr, base + base_addr + u64{nelem} * size);
    for (u32 k = 0; k < nelem; k++) std::memcpy(base + element(k), copy.data() + u64{order[k]} * size, size);
}

} // namespace

// liblv2's _sys_snprintf on a call's variadic arguments after its `fixed` named ones, for the libraries whose own
// snprintf calls _sys_vsnprintf (cellDaisy's cell::Daisy::_snprintf, D-538).
s32 liblv2_snprintf_call(PPUContext& ctx, std::string_view what, u32 dst, u32 count, u32 fmt, u32 fixed)
{
    std::string text;
    if (!format_call(ctx, what, fmt, fixed, text)) return err(lv2::CELL_EFAULT);
    return put_snprintf(ctx, what, dst, count, text);
}

// For sysPrxForUser_sys.cpp and sys_spu_.cpp: liblv2's own _sys_malloc, _sys_memalign and _sys_free, and _sys_panic's
// end.
u32 lv2_malloc(PPUContext& ctx, u32 size) { return _sys_malloc(ctx, size); }
u32 lv2_memalign(PPUContext& ctx, u32 align, u32 size) { return _sys_memalign(ctx, align, size); }
void lv2_free(PPUContext& ctx, u32 addr) { _sys_free(ctx, addr); }
[[noreturn]] void lv2_panic(PPUContext& ctx, std::string_view text) { panic_exit(ctx, text); }

void register_sysPrxForUser_libc(Registry& r)
{
    r.add("sysPrxForUser", {
        PS_HLE_FUNC(0x3EF17F8C, __sys_look_ctype_table),
        PS_HLE_FUNC(0x4B2F301A, _sys_tolower),
        PS_HLE_FUNC(0xEEF75113, _sys_toupper),
        PS_HLE_FUNC(0x68B9B011, _sys_memset),
        PS_HLE_FUNC(0x6BF66EA7, _sys_memcpy),
        PS_HLE_FUNC(0xFB5DB080, _sys_memcmp),
        PS_HLE_FUNC(0x3BD53C7B, _sys_memchr),
        PS_HLE_FUNC(0x27427742, _sys_memmove),
        PS_HLE_FUNC(0x2D36462B, _sys_strlen),
        PS_HLE_FUNC(0x459B4393, _sys_strcmp),
        PS_HLE_FUNC(0x04E83D2C, _sys_strncmp),
        PS_HLE_FUNC(0x052D29A6, _sys_strcat),
        PS_HLE_FUNC(0x7498887B, _sys_strchr),
        PS_HLE_FUNC(0x996F7CF8, _sys_strncat),
        PS_HLE_FUNC(0x99C88692, _sys_strcpy),
        PS_HLE_FUNC(0xD3039D4D, _sys_strncpy),
        PS_HLE_FUNC(0x1CA525A2, _sys_strncasecmp),
        PS_HLE_FUNC(0x191F0C4A, _sys_strrchr),
        PS_HLE_FUNC(0xBDB18F83, _sys_malloc),
        PS_HLE_FUNC(0x318F17E1, _sys_memalign),
        PS_HLE_FUNC(0xF7F7FB20, _sys_free),
        PS_HLE_FUNC(0x06574237, _sys_snprintf),
        PS_HLE_FUNC(0x9F04F7AF, _sys_printf),
        PS_HLE_FUNC(0xA1F9EAFE, _sys_sprintf),
        PS_HLE_FUNC(0x0618936B, _sys_vsnprintf),
        PS_HLE_FUNC(0xFA7F693D, _sys_vprintf),
        PS_HLE_FUNC(0x791B9219, _sys_vsprintf),
        PS_HLE_FUNC(0x4232B0DB, _sys_panic),
        PS_HLE_FUNC(0x9FB6228E, sysPrxForUser_9FB6228E),
        PS_HLE_FUNC(0x8CFEF376, __tls_get_addr),
        PS_HLE_FUNC(0x02E20EC1, __sys_printf_basename),
        PS_HLE_FUNC(0xC4FD6121, _sys_qsort),
        PS_HLE_FUNC(0xB2FCF2C8, _sys_heap_create_heap),
        PS_HLE_FUNC(0xAEDE4B03, _sys_heap_delete_heap),
        PS_HLE_FUNC(0x35168520, _sys_heap_malloc),
        PS_HLE_FUNC(0x44265C08, _sys_heap_memalign),
        PS_HLE_FUNC(0x8A561D92, _sys_heap_free),
        PS_HLE_FUNC(0xB9BF1078, _sys_heap_alloc_heap_memory),
        PS_HLE_FUNC(0xD1AD4570, _sys_heap_get_mallinfo),
        PS_HLE_FUNC(0xB6369393, _sys_heap_get_total_free_size),
        PS_HLE_FUNC(0x8985B5B6, _sys_heap_stats),
    });
    // sys_libc (RPCS3 sys_libc.cpp, REG_FNID by the C names): the same memory functions under libc's names. The
    // firmware has the library in a module of its own that games don't load (docs/firmware.md), but RPCS3 serves it.
    // RPCS3 lists the C library under sys_libm too (D-511); _memset_vmx is memset.
    using detail::thunk;
    for (const std::string_view lib : {std::string_view("sys_libc"), std::string_view("sys_libm")}) {
        r.add(lib, {
            Function{{}, "memcpy", 0x831D70A5, &thunk<&sys_libc_memcpy>, false},
            Function{{}, "memset", 0x5909E3C4, &thunk<&sys_libc_memset>, false},
            Function{{}, "memmove", 0x5B162B7F, &thunk<&sys_libc_memmove>, false},
            Function{{}, "memcmp", 0xC3E14CBE, &thunk<&sys_libc_memcmp>, false},
            Function{{}, "_memset_vmx", recomp::nid_of("_memset_vmx"), &thunk<&sys_libc_memset>, false},
        });
    }
}

} // namespace pspocket::hle
