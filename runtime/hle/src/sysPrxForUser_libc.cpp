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
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/syscalls.hpp"

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

// ------------------------------------------------------------------------------------------------- malloc, heaps

// RPCS3 vm::alloc(size, vm::main, align) with the alignment at least 64 KB: the main area's first fit, zeroed; 0 when
// nothing fits (or for size 0). RPCS3 aborts on an alignment that is not a power of two; refused with a log here.
u32 main_alloc(PPUContext& ctx, std::string_view what, u32 size, u32 align)
{
    align = std::max<u32>(align, 0x10000);
    if (!std::has_single_bit(align)) {
        refuse(ctx, std::format("{}(size {:#x}): alignment {:#x} is not a power of two; NULL", what, size, align));
        return 0;
    }
    HleProcess* p = HleProcess::current();
    const u32 addr = p ? p->main_memory().alloc(size, align) : 0;
    PS_LOG_DEBUG(kChan, "{}(size {:#x}, align {:#x}) -> {:#010x}", what, size, align, addr);
    return addr;
}

// RPCS3 vm::dealloc(addr, vm::main). free(NULL) does nothing; another address that is no allocation is logged by the
// allocator (RPCS3 ignores it).
void main_free(std::string_view what, u32 addr)
{
    PS_LOG_DEBUG(kChan, "{}({:#010x})", what, addr);
    if (HleProcess* p = HleProcess::current(); p && addr) p->main_memory().free(addr);
}

u32 _sys_malloc(PPUContext& ctx, u32 size) { return main_alloc(ctx, "_sys_malloc", size, 0x10000); }

u32 _sys_memalign(PPUContext& ctx, u32 align, u32 size) { return main_alloc(ctx, "_sys_memalign", size, align); }

void _sys_free(PPUContext&, u32 addr) { main_free("_sys_free", addr); }

// RPCS3 sys_heap.cpp: a heap is only a name under an ID (idm's HeapInfo: 1..1023); allocations come from the main area
// as _sys_malloc's whichever heap is named, and deleting a heap or freeing returns CELL_OK whatever the ID.
struct HeapState {
    std::mutex mutex;
    IdTable<std::string, 1, 1023> heaps;
};

std::string guest_name(lv2::PpuThread& t, u32 addr)
{
    if (!addr) return {};
    const std::optional<u64> n = guest_strnlen(t, addr, 0x100);
    return n ? std::string(reinterpret_cast<const char*>(t.kernel.base() + addr), *n) : std::string("?");
}

u32 _sys_heap_create_heap(PPUContext& ctx, u32 name, u32 arg2, u32 arg3, u32 arg4)
{
    std::string heap_name = guest_name(thread_of(ctx), name);
    HeapState& s = HleProcess::current()->state<HeapState>();
    u32 id = 0;
    {
        std::lock_guard lock(s.mutex);
        id = s.heaps.make(heap_name);
    }
    PS_LOG_DEBUG(kChan, "_sys_heap_create_heap(\"{}\", {:#x}, {:#x}, {:#x}) -> {}", heap_name, arg2, arg3, arg4, id);
    if (!id) PS_LOG_ERROR(kChan, "_sys_heap_create_heap(\"{}\"): all 1023 heap IDs in use; 0", heap_name);
    return id;
}

s32 _sys_heap_delete_heap(PPUContext&, u32 heap)
{
    PS_LOG_DEBUG(kChan, "_sys_heap_delete_heap({})", heap);
    HeapState& s = HleProcess::current()->state<HeapState>();
    std::lock_guard lock(s.mutex);
    s.heaps.remove(heap);
    return lv2::CELL_OK;
}

u32 _sys_heap_malloc(PPUContext& ctx, u32, u32 size) { return main_alloc(ctx, "_sys_heap_malloc", size, 0x10000); }

u32 _sys_heap_memalign(PPUContext& ctx, u32, u32 align, u32 size)
{
    return main_alloc(ctx, "_sys_heap_memalign", size, align);
}

s32 _sys_heap_free(PPUContext&, u32, u32 addr)
{
    main_free("_sys_heap_free", addr);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- printf

// The variadic arguments of a call after its `fixed` named ones, as the PPC64 ELF ABI passes them (RPCS3 ps3_fmt_src):
// one 64-bit slot each, argument n in r(3 + n) for n < 8, else in the caller's parameter save area at r1 + 0x30 + 8n.
// A double is in its slot as well (a variadic call puts it there besides an FPR), which is where RPCS3 reads it too.
// Deviation: RPCS3 reads the ninth argument and the ones after it one slot low (it passes its 0-based index to
// get_stack_arg, which counts from 1); here from where the caller put them.
class VarArgs {
public:
    VarArgs(PPUContext& ctx, u32 fixed) : ctx_(ctx), sp_(static_cast<u32>(ctx.gpr[1])), next_(fixed) {}

    u64 next()
    {
        const u32 n = next_++;
        if (n < 8) return ctx_.gpr[3 + n];
        const u64 slot = u64{sp_} + 0x30 + 8 * u64{n};
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
    bool unreadable_ = false;
};

// A conversion's flags, width, precision and length (RPCS3 cfmt_append's cfmt_context).
struct Conversion {
    bool left = false, sign = false, space = false, alter = false, zeros = false, dot = false;
    u32 width = 0, prec = 0;
    u32 size = 0;            // the argument's bytes by the length modifier (ps3_fmt_src: long, size_t 4); 0 if none
    bool plain_long = false; // the modifier was one 'l'
};

// Widths and precisions are cut to this, so a "%999999999d" cannot exhaust the host's memory.
constexpr u32 kMaxWidth = 0x10000;

// `v` in `radix`, at least `prec` digits with a precision, else at least one; none for 0 at precision 0 (C99).
std::string digits(u64 v, u32 radix, bool upper, const Conversion& c, u32 min_digits = 1)
{
    const char* set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string d;
    for (; v; v /= radix) d += set[v % radix];
    const u32 want = c.dot ? c.prec : min_digits;
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

// RPCS3 formats the floating conversions with the host's snprintf, the sequence as written; the same here, with the
// width and precision a '*' gave written in (RPCS3 passes those arguments in an order only some hosts read right).
std::string format_double(const Conversion& c, char conv, double v)
{
    std::string spec = "%";
    if (c.left) spec += '-';
    if (c.sign) spec += '+';
    if (c.space) spec += ' ';
    if (c.alter) spec += '#';
    if (c.zeros) spec += '0';
    if (c.width) spec += std::to_string(c.width);
    if (c.dot) spec += '.' + std::to_string(c.prec);
    spec += conv;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral" // built above from the parsed flags only
    const int n = std::snprintf(nullptr, 0, spec.c_str(), v);
    std::string out(n > 0 ? static_cast<usize>(n) : 0, '\0');
    if (n > 0) std::snprintf(out.data(), out.size() + 1, spec.c_str(), v);
#pragma clang diagnostic pop
    return out;
}

// RPCS3 sys_libc_.cpp ps3_fmt: Utilities/cfmt.h cfmt_append over the guest's arguments. Its parsing is kept: flags,
// width, precision and length in any order, "%%" only right after the '%', and a sequence it does not know (or a second
// width, precision or length) printed as written with the rest of the format after it untouched. Conversions: d i u o x
// X c s p, RPCS3's b and B (binary), and f F e E g G a A; h hh l ll z j t. What differs (C99 is what liblv2 follows):
// - d and i take the argument at the conversion's size and sign (RPCS3 takes %d's whole 64-bit slot), as u, o and x
//   always did in both.
// - Precision 0 prints no digits only for the value 0, and '#' gives "0x" and a leading 0 as C99 (RPCS3 prints nothing
//   for "%.0d" of any value, and "00" for "%#x" or "%#o" of 0).
// - %lf is %f (RPCS3 stops at it); %Lf stays unknown.
// - %s of a null pointer prints "(null)"; a %s or stack argument that is not guest memory is noted in `problems` and
//   printed as nothing or 0 (RPCS3 faults).
// - %p is RPCS3's: 16 hex digits without "0x".
std::string format_args(lv2::PpuThread& t, std::string_view fmt, VarArgs& args, std::vector<std::string>& problems)
{
    std::string out;
    usize i = 0;
    while (i < fmt.size()) {
        const char first = fmt[i++];
        if (first != '%') {
            out += first;
            continue;
        }
        const usize start = i - 1;
        if (i < fmt.size() && fmt[i] == '%') {
            out += '%';
            i++;
            continue;
        }
        const auto decimal = [&](u32 v) {
            u64 r = v;
            while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9')
                r = std::min<u64>(r * 10 + static_cast<u64>(fmt[i++] - '0'), kMaxWidth);
            return static_cast<u32>(r);
        };
        Conversion c;
        bool done = false, dropped = false;
        while (!done && !dropped) {
            if (i == fmt.size()) { // an unfinished sequence
                dropped = true;
                break;
            }
            const char ch = fmt[i++];
            switch (ch) {
            case '-': c.left = true; break;
            case '+': c.sign = true; break;
            case ' ': c.space = true; break;
            case '#': c.alter = true; break;
            case '0': c.zeros = true; break;
            case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
                if (c.width) dropped = true;
                else c.width = decimal(static_cast<u32>(ch - '0'));
                break;
            case '*': {
                if (c.width) {
                    dropped = true;
                    break;
                }
                const s32 w = static_cast<s32>(args.next());
                c.width = std::min<u32>(w < 0 ? 0u - static_cast<u32>(w) : static_cast<u32>(w), kMaxWidth);
                c.left |= w < 0;
                break;
            }
            case '.':
                if (c.dot || c.prec) {
                    dropped = true;
                } else if (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') {
                    c.prec = decimal(0);
                    c.dot = true;
                } else if (i < fmt.size() && fmt[i] == '*') {
                    i++;
                    const s32 p = static_cast<s32>(args.next());
                    c.prec = p > 0 ? std::min<u32>(static_cast<u32>(p), kMaxWidth) : 0;
                    c.dot = p >= 0;
                } else {
                    c.dot = true;
                }
                break;
            case 'h':
                if (c.size) dropped = true;
                else if (i < fmt.size() && fmt[i] == 'h') c.size = 1, i++;
                else c.size = 2;
                break;
            case 'l':
                if (c.size) dropped = true;
                else if (i < fmt.size() && fmt[i] == 'l') c.size = 8, i++;
                else c.size = 4, c.plain_long = true;
                break;
            case 'z':
            case 't':
                if (c.size) dropped = true;
                else c.size = 4;
                break;
            case 'j':
                if (c.size) dropped = true;
                else c.size = 8;
                break;
            case 'c': {
                if (c.size) {
                    dropped = true;
                    break;
                }
                const char v = static_cast<char>(args.next() & 0xFF);
                put_padded(out, c, "", std::string_view(&v, 1), false);
                done = true;
                break;
            }
            case 's': {
                if (c.size) {
                    dropped = true;
                    break;
                }
                const u32 addr = static_cast<u32>(args.next());
                std::string s;
                if (!addr) {
                    s = "(null)";
                } else if (const std::optional<u64> n = guest_strnlen(t, addr, c.dot ? c.prec : kMaxString)) {
                    s.assign(reinterpret_cast<const char*>(t.kernel.base() + addr), *n);
                } else {
                    problems.push_back(
                        std::format("the %s argument {:#x} is not guest memory (printed as nothing)", addr));
                }
                if (c.dot && s.size() > c.prec) s.resize(c.prec);
                put_padded(out, c, "", s, false);
                done = true;
                break;
            }
            case 'd':
            case 'i': {
                const u64 raw = args.next();
                const u32 size = c.size ? c.size : 4;
                const s64 v = size == 1 ? s64{static_cast<s8>(raw)} :
                    size == 2           ? s64{static_cast<s16>(raw)} :
                    size == 4           ? s64{static_cast<s32>(raw)} :
                                          static_cast<s64>(raw);
                const u64 magnitude = v < 0 ? 0 - static_cast<u64>(v) : static_cast<u64>(v);
                put_padded(out, c, v < 0 ? "-" : c.sign ? "+" : c.space ? " " : "", digits(magnitude, 10, false, c),
                    !c.dot);
                done = true;
                break;
            }
            case 'u': case 'o': case 'x': case 'X': case 'b': case 'B': {
                const u64 raw = args.next();
                const u32 size = c.size ? c.size : 4;
                const u64 v = size == 8 ? raw : raw & ((u64{1} << (8 * size)) - 1);
                std::string_view prefix;
                std::string body;
                if (ch == 'u') {
                    body = digits(v, 10, false, c);
                } else if (ch == 'o') {
                    body = digits(v, 8, false, c);
                    if (c.alter && (body.empty() || body[0] != '0')) body.insert(body.begin(), '0');
                } else if (ch == 'x' || ch == 'X') {
                    body = digits(v, 16, ch == 'X', c);
                    if (c.alter && v) prefix = ch == 'x' ? "0x" : "0X";
                } else {
                    body = digits(v, 2, false, c);
                }
                put_padded(out, c, prefix, body, !c.dot);
                done = true;
                break;
            }
            case 'p': {
                if (c.size) {
                    dropped = true;
                    break;
                }
                Conversion hex16 = c;
                hex16.dot = false;
                put_padded(out, c, "", digits(args.next(), 16, false, hex16, 16), false);
                done = true;
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
                if (c.size && !c.plain_long) {
                    dropped = true;
                    break;
                }
                out += format_double(c, ch, std::bit_cast<double>(args.next()));
                done = true;
                break;
            default: // 'L', 'n' and anything else: unknown, as RPCS3
                dropped = true;
                break;
            }
        }
        if (dropped) {
            out.append(fmt.substr(start));
            break;
        }
    }
    return out;
}

// The text of a printf-family call: the guest format at `fmt` with the arguments after the call's `fixed` named ones.
// False (the call site logged) when the format is not guest memory: RPCS3 faults, the call returns CELL_EFAULT here.
bool format_call(PPUContext& ctx, std::string_view what, u32 fmt, u32 fixed, std::string& out)
{
    lv2::PpuThread& t = thread_of(ctx);
    const std::optional<u64> n = fmt ? guest_strnlen(t, fmt, kMaxString) : std::nullopt;
    if (!n) {
        refuse(ctx, std::format("{}: the format {:#x} is not guest memory; CELL_EFAULT", what, fmt));
        return false;
    }
    const std::string format(reinterpret_cast<const char*>(t.kernel.base() + fmt), *n);
    VarArgs args(ctx, fixed);
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

// RPCS3 _sys_printf: the text to sys_tty_write on channel 0, from guest memory (vm::make_str puts it on the stack).
// Here in pieces of up to 1 KB on the caller's stack, each ending after a line where one ends in it, so a long text
// needs no more stack than that.
s32 _sys_printf(PPUContext& ctx, u32 fmt)
{
    std::string text;
    if (!format_call(ctx, "_sys_printf", fmt, 1, text)) return err(lv2::CELL_EFAULT);
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
    return lv2::CELL_OK;
}

// RPCS3: the text and its NUL into `buffer`; its length.
s32 _sys_sprintf(PPUContext& ctx, u32 buffer, u32 fmt)
{
    std::string text;
    if (!format_call(ctx, "_sys_sprintf", fmt, 2, text)) return err(lv2::CELL_EFAULT);
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_range(t, buffer, text.size() + 1)) {
        refuse(ctx, std::format("_sys_sprintf({:#x}): {} bytes do not fit guest memory there; CELL_EFAULT", buffer,
            text.size() + 1));
        return err(lv2::CELL_EFAULT);
    }
    std::memcpy(t.kernel.base() + buffer, text.data(), text.size());
    t.kernel.base()[buffer + text.size()] = 0;
    return static_cast<s32>(text.size());
}

// RPCS3: 0 for a count of 0 (nothing written); else up to count - 1 bytes of the text and a NUL, and the number of
// bytes written (not the text's whole length, which C99's snprintf returns).
s32 _sys_snprintf(PPUContext& ctx, u32 dst, u32 count, u32 fmt)
{
    std::string text;
    if (!format_call(ctx, "_sys_snprintf", fmt, 3, text)) return err(lv2::CELL_EFAULT);
    if (!count) return 0;
    const u32 n = static_cast<u32>(std::min<usize>(count - 1, text.size()));
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_range(t, dst, u64{n} + 1)) {
        refuse(ctx, std::format("_sys_snprintf({:#x}, {}): not guest memory; CELL_EFAULT", dst, count));
        return err(lv2::CELL_EFAULT);
    }
    std::memcpy(t.kernel.base() + dst, text.data(), n);
    t.kernel.base()[dst + n] = 0;
    return static_cast<s32>(n);
}

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
        PS_HLE_FUNC(0xC4FD6121, _sys_qsort),
        PS_HLE_FUNC(0xB2FCF2C8, _sys_heap_create_heap),
        PS_HLE_FUNC(0xAEDE4B03, _sys_heap_delete_heap),
        PS_HLE_FUNC(0x35168520, _sys_heap_malloc),
        PS_HLE_FUNC(0x44265C08, _sys_heap_memalign),
        PS_HLE_FUNC(0x8A561D92, _sys_heap_free),
    });
}

} // namespace pspocket::hle
