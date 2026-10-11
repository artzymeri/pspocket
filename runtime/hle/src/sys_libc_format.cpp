// sys_libc part 7 (D-905): printf's internals as the SDK's C library has them, under sys_libc and sys_libm, on the
// engine of libc_printf.hpp working in guest memory (a game calls them with its own buffers and output function):
// - _Printf and _WPrintf (pfn(arg, text, n) gets each piece as the library puts them out, its answer the next arg,
//   NULL stopping it: EOF), and their steps on the caller's print-formatting structure: _Putfld, _Puttxt, _Putstr,
//   _Litob, _Ldtob, _Genld and the wide _WPutfld, _WPuttxt, _WPutstr, _WLitob, _WLdtob, _WGenld.
//   _Pft (narrow) as the game's library lays it out: +0 the value (long long or double), +16 a vector argument, +32
//   pfn, +36 arg, +40 s, +44 n0, nz0, n1, nz1, n2, nz2 (to +64), +68 prec, +72 nchar, +76 width, +80 flags (u16),
//   +82 qual (char). _WPft: +0 the value, +8 pfn, +12 arg, +16 s, +20 n0 .. nz2 (to +40), +44 nchar, +48 prec,
//   +52 width, +56 flags (u16), +60 qual (wchar_t): the game read was built with a 16-bit wchar_t (qual at +58 there);
//   sys_libc's wchar_t is 32 bits (D-512), its wide text 32-bit words.
// - The SPU printf (spu_printf's server, spu_thread_printf and kin): __spu_thread_printf(pfn, arg, spu, ls_args) walks
//   a format in the SPU thread's local store (its address the first quadword of ls_args, the arguments one a
//   quadword after it), literal characters to pfn one at a time; flags " +-#0", qualifiers h hh l ll j t z L;
//   __spu_thread_putfld(pft, spu, &ap, code, ac) reads each argument from the store (int, long and pointer the word
//   at ap, long long and double the doubleword), every conversion taking a quadword (%% too); %s's text stays in the
//   store (flag 0x20, the SPU at +84) and __spu_thread_puttxt(pft, ac) reads it 128 bytes at a time ("(null)" for
//   address 0); %n writes the count into the store. The __raw_spu_ forms do the same on a raw SPU's store (guest
//   memory at 0xE0000000 + 0x100000 * id; no executable seen links them: they follow the SPU threads' ones).
//   spu_thread_printf (TTY channel 2, the SPU's), spu_thread_sprintf/snprintf, raw_spu_printf, spu_raw_sprintf and
//   spu_raw_snprintf run on them.

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "libc_format.hpp"
#include "libc_guest.hpp"
#include "libc_malloc.hpp"
#include "libc_spu_printf.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/spu/context.h"

namespace pspocket::hle {
namespace {

using namespace libc;

u8* gb(PPUContext& ctx) { return guest_base(ctx); }

// ------------------------------------------------------------------------------------------------- guest text

// A pointer to guest characters: bytes, or (C = char32_t) 32-bit big-endian words. Each access is checked: a read
// outside guest memory gives 0, a write there is dropped (and noted).
template <class C>
class GPtr {
public:
    static constexpr u32 kUnit = sizeof(C) == 1 ? 1 : 4;
    GPtr() = default;
    GPtr(PPUContext* ctx, u32 addr) : ctx_(ctx), addr_(addr) {}
    u32 addr() const { return addr_; }

    class Ref {
    public:
        Ref(PPUContext* ctx, u32 addr) : ctx_(ctx), addr_(addr) {}
        Ref& operator=(C c)
        {
            if (!ok()) return *this;
            if constexpr (sizeof(C) == 1) gb(*ctx_)[addr_] = static_cast<u8>(c);
            else ps_write32(gb(*ctx_), addr_, static_cast<u32>(c));
            return *this;
        }
        Ref& operator=(const Ref& r) { return *this = static_cast<C>(r); }
        operator C() const
        {
            if (!ok()) return 0;
            if constexpr (sizeof(C) == 1) return static_cast<C>(gb(*ctx_)[addr_]);
            else return static_cast<C>(ps_read32(gb(*ctx_), addr_));
        }

    private:
        bool ok() const
        {
            if (ctx_ && guest_range(thread_of(*ctx_), addr_, kUnit)) return true;
            if (ctx_) refuse(*ctx_, std::format("printf: [{:#x}, +{}) is not guest memory", addr_, kUnit));
            return false;
        }
        PPUContext* ctx_;
        u32 addr_;
    };

    Ref operator[](s32 k) const { return Ref(ctx_, addr_ + static_cast<u32>(k) * kUnit); }
    Ref operator*() const { return (*this)[0]; }
    GPtr operator+(s32 k) const { return GPtr(ctx_, addr_ + static_cast<u32>(k) * kUnit); }
    GPtr operator-(s32 k) const { return GPtr(ctx_, addr_ - static_cast<u32>(k) * kUnit); }

private:
    PPUContext* ctx_ = nullptr;
    u32 addr_ = 0;
};

// The library's constant strings a field may put out, in guest memory (once per process): 32 spaces and 32 zeros
// (_Puttxt's padding, 31 of each for the wide one) and "(null)".
struct FormatConstants {
    std::once_flag once;
    u32 spaces = 0, zeros = 0, wspaces = 0, wzeros = 0, null = 0, wnull = 0;
};

FormatConstants& constants(PPUContext& ctx)
{
    FormatConstants& c = HleProcess::current()->state<FormatConstants>();
    std::call_once(c.once, [&] {
        const u32 a = HleProcess::current()->main_memory().alloc(0x200, 16);
        if (!a) return;
        u8* b = gb(ctx);
        c.spaces = a;
        c.zeros = a + 0x20;
        c.null = a + 0x40;
        c.wspaces = a + 0x50;
        c.wzeros = a + 0xD0;
        c.wnull = a + 0x150;
        std::memset(b + c.spaces, ' ', 32);
        std::memset(b + c.zeros, '0', 32);
        std::memcpy(b + c.null, "(null)", 7);
        for (u32 i = 0; i < 32; i++) {
            ps_write32(b, c.wspaces + 4 * i, i < 31 ? ' ' : 0);
            ps_write32(b, c.wzeros + 4 * i, i < 31 ? '0' : 0);
        }
        const char* n = "(null)";
        for (u32 i = 0; i < 7; i++) ps_write32(b, c.wnull + 4 * i, static_cast<u8>(n[i]));
    });
    return c;
}

// A sink calling the guest's pfn(arg, text, n) for each piece (padding 32 characters a time, 31 for the wide one, as
// the library's _Puttxt and _WPuttxt); NULL from it stops the field. The next call still goes out with arg NULL
// where the library goes on (_Putstr's flushes).
template <class C>
class GuestSink {
public:
    GuestSink(PPUContext& ctx, u32 pfn, u32 arg) : ctx_(ctx), pfn_(pfn), arg_(arg) {}
    template <class PX>
    bool put(PX& px, GPtr<C> p, s32 n)
    {
        arg_ = static_cast<u32>(call_guest(ctx_, pfn_, {arg_, p.addr(), static_cast<u64>(static_cast<u32>(n))}));
        if (!arg_) return false;
        px.nchar = add32(px.nchar, n);
        return true;
    }
    template <class PX>
    bool pad(PX& px, C c, s32 n)
    {
        const FormatConstants& k = constants(ctx_);
        constexpr s32 chunk = sizeof(C) == 1 ? 32 : 31;
        const u32 from = sizeof(C) == 1 ? (c == ' ' ? k.spaces : k.zeros) : (c == ' ' ? k.wspaces : k.wzeros);
        while (0 < n) {
            const s32 m = std::min(n, chunk);
            if (!put(px, GPtr<C>(&ctx_, from), m)) return false;
            n -= m;
        }
        return true;
    }
    u32 arg() const { return arg_; }

private:
    PPUContext& ctx_;
    u32 pfn_, arg_;
};

// The arguments of a guest va_list (the address of the next argument's 64-bit slot; a vector at the next 16-byte
// boundary).
class ListArgs final : public FormatSource {
public:
    ListArgs(PPUContext& ctx, u32 ap) : ctx_(ctx), ap_(ap) {}
    u64 next() override
    {
        const u32 a = ap_;
        ap_ += 8;
        if (!guest_range(thread_of(ctx_), a, 8)) {
            problems.push_back(std::format("the argument at {:#x} is not guest memory (0 taken)", a));
            return 0;
        }
        return ps_read64(gb(ctx_), a);
    }
    std::array<u8, 16> next_vector() override
    {
        ap_ = (ap_ + 15) & ~15u;
        std::array<u8, 16> v{};
        if (guest_range(thread_of(ctx_), ap_, 16)) std::memcpy(v.data(), gb(ctx_) + ap_, 16);
        else problems.push_back(std::format("the vector argument at {:#x} is not guest memory (0 taken)", ap_));
        ap_ += 16;
        return v;
    }
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
    u32 ap() const { return ap_; }

private:
    PPUContext& ctx_;
    u32 ap_;
};

// The engine's memory policy in guest memory: a %s argument's text where it lies, "(null)" from the constants,
// _Putstr's buffer on the stack (64 characters) or from malloc (bigger, as the library's).
template <class C>
class GuestText {
public:
    GuestText(PPUContext& ctx, FormatSource& src) : ctx_(ctx), src_(src) {}
    ~GuestText() { release(); }
    GuestText(const GuestText&) = delete;
    GuestText& operator=(const GuestText&) = delete;

    FormatSource& src() { return src_; }
    bool text_arg(u32 addr, s32 prec, GPtr<C>& s, s32& n)
    {
        const u64 max = prec < 0 ? kNoLimit : static_cast<u64>(prec);
        std::optional<u64> len;
        if constexpr (sizeof(C) == 1) {
            len = guest_strnlen(thread_of(ctx_), addr, max);
        } else {
            const auto w = read_wstring(thread_of(ctx_), addr, max);
            if (w) len = w->size();
        }
        if (!len) {
            src_.problems.push_back(std::format("the text argument at {:#x} is not guest memory (printed as nothing)", addr));
            return false;
        }
        s = GPtr<C>(&ctx_, addr);
        n = static_cast<s32>(std::min<u64>(*len, 0x7FFFFFFF));
        return true;
    }
    GPtr<C> null_text()
    {
        const FormatConstants& k = constants(ctx_);
        return GPtr<C>(&ctx_, sizeof(C) == 1 ? k.null : k.wnull);
    }
    std::optional<std::u32string> wide_chars(u32 addr, u64 max)
    {
        auto r = read_wstring(thread_of(ctx_), addr, std::min<u64>(max, kNoLimit));
        if (!r) src_.problems.push_back(std::format("the wide text at {:#x} is not guest memory (printed as nothing)", addr));
        return r;
    }
    std::optional<std::string> narrow_chars(u32 addr, u64 max)
    {
        auto r = read_string(thread_of(ctx_), addr, std::min<u64>(max, kNoLimit));
        if (!r) src_.problems.push_back(std::format("the text at {:#x} is not guest memory (printed as nothing)", addr));
        return r;
    }
    std::optional<GPtr<C>> scratch(u32 size, u32 need)
    {
        (void)need;
        release();
        constexpr u32 unit = GPtr<C>::kUnit;
        if (size <= 64) {
            stack_ = std::make_unique<StackVar>(ctx_, gb(ctx_), 64 * unit);
            return GPtr<C>(&ctx_, stack_->addr());
        }
        if (u64{size} * unit > 0xFFFFFFFFull) return std::nullopt;
        heap_ = lib_malloc(ctx_, size * unit);
        if (!heap_) return std::nullopt;
        return GPtr<C>(&ctx_, heap_);
    }
    bool store(u32 addr, u64 value, u32 bytes) { return src_.store(addr, value, bytes); }

private:
    void release()
    {
        stack_.reset();
        if (heap_) lib_free(ctx_, heap_, "printf");
        heap_ = 0;
    }
    PPUContext& ctx_;
    FormatSource& src_;
    std::unique_ptr<StackVar> stack_;
    u32 heap_ = 0;
};

void note_problems(PPUContext& ctx, const char* fn, const std::vector<std::string>& problems)
{
    for (const std::string& p : problems) refuse(ctx, std::format("{}: {}", fn, p));
}

// ------------------------------------------------------------------------------------------------- the structures

namespace pft {
inline constexpr u32 kSize = 0x54, kSpuSize = 0x58, kWideSize = 0x40;
} // namespace pft

template <class C>
struct GuestPft {
    Pft<C, GPtr<C>> px;
    u32 pfn = 0, arg = 0;
    u32 ls_text = 0, spu = 0; // the SPU printf's
};

// Narrow _Pft (and the SPU printf's, with its SPU at +84).
GuestPft<char> load_pft(PPUContext& ctx, u32 a, bool spu = false)
{
    u8* b = gb(ctx);
    GuestPft<char> g;
    Pft<char, GPtr<char>>& px = g.px;
    px.v = ps_read64(b, a);
    std::memcpy(px.vec.data(), b + a + 0x10, 16);
    g.pfn = ps_read32(b, a + 0x20);
    g.arg = ps_read32(b, a + 0x24);
    px.s = GPtr<char>(&ctx, ps_read32(b, a + 0x28));
    g.ls_text = ps_read32(b, a + 0x28);
    px.n0 = static_cast<s32>(ps_read32(b, a + 0x2C));
    px.nz0 = static_cast<s32>(ps_read32(b, a + 0x30));
    px.n1 = static_cast<s32>(ps_read32(b, a + 0x34));
    px.nz1 = static_cast<s32>(ps_read32(b, a + 0x38));
    px.n2 = static_cast<s32>(ps_read32(b, a + 0x3C));
    px.nz2 = static_cast<s32>(ps_read32(b, a + 0x40));
    px.prec = static_cast<s32>(ps_read32(b, a + 0x44));
    px.nchar = static_cast<s32>(ps_read32(b, a + 0x48));
    px.width = static_cast<s32>(ps_read32(b, a + 0x4C));
    px.flags = ps_read16(b, a + 0x50);
    px.qual = static_cast<char>(b[a + 0x52]);
    if (spu) g.spu = ps_read32(b, a + 0x54);
    return g;
}

void store_pft(PPUContext& ctx, u32 a, const GuestPft<char>& g, bool spu = false)
{
    u8* b = gb(ctx);
    const Pft<char, GPtr<char>>& px = g.px;
    ps_write64(b, a, px.v);
    std::memcpy(b + a + 0x10, px.vec.data(), 16);
    ps_write32(b, a + 0x24, g.arg);
    ps_write32(b, a + 0x28, (spu && (px.flags & kFlsText)) ? g.ls_text : px.s.addr());
    ps_write32(b, a + 0x2C, static_cast<u32>(px.n0));
    ps_write32(b, a + 0x30, static_cast<u32>(px.nz0));
    ps_write32(b, a + 0x34, static_cast<u32>(px.n1));
    ps_write32(b, a + 0x38, static_cast<u32>(px.nz1));
    ps_write32(b, a + 0x3C, static_cast<u32>(px.n2));
    ps_write32(b, a + 0x40, static_cast<u32>(px.nz2));
    ps_write32(b, a + 0x44, static_cast<u32>(px.prec));
    ps_write32(b, a + 0x48, static_cast<u32>(px.nchar));
    ps_write32(b, a + 0x4C, static_cast<u32>(px.width));
    ps_write16(b, a + 0x50, px.flags);
    b[a + 0x52] = static_cast<u8>(px.qual);
    if (spu) ps_write32(b, a + 0x54, g.spu);
}

GuestPft<char32_t> load_wpft(PPUContext& ctx, u32 a)
{
    u8* b = gb(ctx);
    GuestPft<char32_t> g;
    Pft<char32_t, GPtr<char32_t>>& px = g.px;
    px.v = ps_read64(b, a);
    g.pfn = ps_read32(b, a + 8);
    g.arg = ps_read32(b, a + 12);
    px.s = GPtr<char32_t>(&ctx, ps_read32(b, a + 16));
    px.n0 = static_cast<s32>(ps_read32(b, a + 20));
    px.nz0 = static_cast<s32>(ps_read32(b, a + 24));
    px.n1 = static_cast<s32>(ps_read32(b, a + 28));
    px.nz1 = static_cast<s32>(ps_read32(b, a + 32));
    px.n2 = static_cast<s32>(ps_read32(b, a + 36));
    px.nz2 = static_cast<s32>(ps_read32(b, a + 40));
    px.nchar = static_cast<s32>(ps_read32(b, a + 44));
    px.prec = static_cast<s32>(ps_read32(b, a + 48));
    px.width = static_cast<s32>(ps_read32(b, a + 52));
    px.flags = ps_read16(b, a + 56);
    px.qual = static_cast<char32_t>(ps_read32(b, a + 60));
    return g;
}

void store_wpft(PPUContext& ctx, u32 a, const GuestPft<char32_t>& g)
{
    u8* b = gb(ctx);
    const Pft<char32_t, GPtr<char32_t>>& px = g.px;
    ps_write64(b, a, px.v);
    ps_write32(b, a + 12, g.arg);
    ps_write32(b, a + 16, px.s.addr());
    ps_write32(b, a + 20, static_cast<u32>(px.n0));
    ps_write32(b, a + 24, static_cast<u32>(px.nz0));
    ps_write32(b, a + 28, static_cast<u32>(px.n1));
    ps_write32(b, a + 32, static_cast<u32>(px.nz1));
    ps_write32(b, a + 36, static_cast<u32>(px.n2));
    ps_write32(b, a + 40, static_cast<u32>(px.nz2));
    ps_write32(b, a + 44, static_cast<u32>(px.nchar));
    ps_write32(b, a + 48, static_cast<u32>(px.prec));
    ps_write32(b, a + 52, static_cast<u32>(px.width));
    ps_write16(b, a + 56, px.flags);
    ps_write32(b, a + 60, static_cast<u32>(px.qual));
}

// ------------------------------------------------------------------------------------------------- _Printf, _WPrintf

template <class C>
s32 printf_guest(PPUContext& ctx, const char* fn, u32 pfn, u32 arg, u32 fmt, u32 ap)
{
    std::optional<std::basic_string<C>> f;
    if constexpr (sizeof(C) == 1) f = read_string(thread_of(ctx), fmt);
    else f = read_wstring(thread_of(ctx), fmt, kMaxString / 4);
    if (!f) {
        refuse(ctx, std::format("{}: the format at {:#x} runs out of guest memory", fn, fmt));
        return -1;
    }
    StackVar ac(ctx, gb(ctx), 128 * GPtr<C>::kUnit);
    ListArgs args(ctx, ap);
    GuestText<C> text(ctx, args);
    GuestSink<C> out(ctx, pfn, arg);
    const s32 r = print_format<C>(std::basic_string_view<C>(*f), GPtr<C>(&ctx, fmt), text, out, GPtr<C>(&ctx, ac.addr()));
    note_problems(ctx, fn, args.problems);
    return r;
}

// int _Printf(void *(*pfn)(void *, const char *, size_t), void *arg, const char *fmt, va_list ap)
s32 _Printf_(PPUContext& ctx, u32 pfn, u32 arg, u32 fmt, u32 ap) { return printf_guest<char>(ctx, "_Printf", pfn, arg, fmt, ap); }
// int _WPrintf(void *(*pfn)(void *, const wchar_t *, size_t), void *arg, const wchar_t *fmt, va_list ap)
s32 _WPrintf_(PPUContext& ctx, u32 pfn, u32 arg, u32 fmt, u32 ap) { return printf_guest<char32_t>(ctx, "_WPrintf", pfn, arg, fmt, ap); }

// ------------------------------------------------------------------------------------------------- the steps

// int _Putfld(_Pft *px, va_list *pap, char code, char *ac)
s32 _Putfld_(PPUContext& ctx, u32 pf, u32 pap, s32 code, u32 ac)
{
    if (!range_ok(ctx, "_Putfld", pf, pft::kSize) || !range_ok(ctx, "_Putfld", pap, 4)) return -1;
    GuestPft<char> g = load_pft(ctx, pf);
    ListArgs args(ctx, ps_read32(gb(ctx), pap));
    s32 r;
    {
        GuestText<char> text(ctx, args);
        GuestSink<char> out(ctx, g.pfn, g.arg);
        r = putfld(g.px, text, static_cast<char>(code), GPtr<char>(&ctx, ac), out);
        g.arg = out.arg();
    }
    ps_write32(gb(ctx), pap, args.ap());
    store_pft(ctx, pf, g);
    note_problems(ctx, "_Putfld", args.problems);
    return r;
}

// int _Puttxt(_Pft *px, const char *ac)
s32 _Puttxt_(PPUContext& ctx, u32 pf, u32 ac)
{
    if (!range_ok(ctx, "_Puttxt", pf, pft::kSize)) return -1;
    GuestPft<char> g = load_pft(ctx, pf);
    GuestSink<char> out(ctx, g.pfn, g.arg);
    const s32 r = puttxt(g.px, GPtr<char>(&ctx, ac), out) ? 0 : -1;
    g.arg = out.arg();
    store_pft(ctx, pf, g);
    return r;
}

// The wide text _Putstr converts: at most the precision of characters (a pointer that is not memory: none, noted).
std::u32string wide_arg(PPUContext& ctx, const char* fn, u32 pwc, s32 prec, bool& terminated)
{
    const u64 limit = prec < 0 ? kNoLimit : static_cast<u64>(prec);
    auto w = read_wstring(thread_of(ctx), pwc, limit);
    if (!w) {
        refuse(ctx, std::format("{}: the wide text at {:#x} is not guest memory", fn, pwc));
        terminated = true;
        return {};
    }
    terminated = w->size() < limit;
    return std::move(*w);
}

// int _Putstr(_Pft *px, const wchar_t *pwc)
s32 _Putstr_(PPUContext& ctx, u32 pf, u32 pwc)
{
    if (!range_ok(ctx, "_Putstr", pf, pft::kSize)) return -1;
    GuestPft<char> g = load_pft(ctx, pf);
    bool terminated = true;
    const std::u32string w = wide_arg(ctx, "_Putstr", pwc, g.px.prec, terminated);
    ListArgs none(ctx, 0);
    s32 r;
    {
        GuestText<char> text(ctx, none);
        GuestSink<char> out(ctx, g.pfn, g.arg);
        r = putstr(g.px, w, terminated, out, text);
        g.arg = out.arg();
    }
    store_pft(ctx, pf, g);
    return r;
}

// void _Litob(_Pft *px, char code)
void _Litob_(PPUContext& ctx, u32 pf, s32 code)
{
    if (!range_ok(ctx, "_Litob", pf, pft::kSize)) return;
    GuestPft<char> g = load_pft(ctx, pf);
    litob(g.px, static_cast<char>(code));
    store_pft(ctx, pf, g);
}

// void _Ldtob(_Pft *px, char code)
void _Ldtob_(PPUContext& ctx, u32 pf, s32 code)
{
    if (!range_ok(ctx, "_Ldtob", pf, pft::kSize)) return;
    GuestPft<char> g = load_pft(ctx, pf);
    ldtob(g.px, static_cast<char>(code));
    store_pft(ctx, pf, g);
}

// void _Genld(_Pft *px, char code, const char *p, short nsig, short xexp)
void _Genld_(PPUContext& ctx, u32 pf, s32 code, u32 p, s32 nsig, s32 xexp)
{
    if (!range_ok(ctx, "_Genld", pf, pft::kSize)) return;
    GuestPft<char> g = load_pft(ctx, pf);
    genld(g.px, static_cast<char>(code), GPtr<char>(&ctx, p), static_cast<s16>(nsig), static_cast<s16>(xexp));
    store_pft(ctx, pf, g);
}

// int _WPutfld(_WPft *px, va_list *pap, wchar_t code, wchar_t *ac)
s32 _WPutfld_(PPUContext& ctx, u32 pf, u32 pap, u32 code, u32 ac)
{
    if (!range_ok(ctx, "_WPutfld", pf, pft::kWideSize) || !range_ok(ctx, "_WPutfld", pap, 4)) return -1;
    GuestPft<char32_t> g = load_wpft(ctx, pf);
    ListArgs args(ctx, ps_read32(gb(ctx), pap));
    s32 r;
    {
        GuestText<char32_t> text(ctx, args);
        GuestSink<char32_t> out(ctx, g.pfn, g.arg);
        r = wputfld(g.px, text, static_cast<char32_t>(code), GPtr<char32_t>(&ctx, ac), out);
        g.arg = out.arg();
    }
    ps_write32(gb(ctx), pap, args.ap());
    store_wpft(ctx, pf, g);
    note_problems(ctx, "_WPutfld", args.problems);
    return r;
}

// int _WPuttxt(_WPft *px, const wchar_t *ac)
s32 _WPuttxt_(PPUContext& ctx, u32 pf, u32 ac)
{
    if (!range_ok(ctx, "_WPuttxt", pf, pft::kWideSize)) return -1;
    GuestPft<char32_t> g = load_wpft(ctx, pf);
    GuestSink<char32_t> out(ctx, g.pfn, g.arg);
    const s32 r = puttxt(g.px, GPtr<char32_t>(&ctx, ac), out) ? 0 : -1;
    g.arg = out.arg();
    store_wpft(ctx, pf, g);
    return r;
}

// int _WPutstr(_WPft *px, const char *pc)
s32 _WPutstr_(PPUContext& ctx, u32 pf, u32 pc)
{
    if (!range_ok(ctx, "_WPutstr", pf, pft::kWideSize)) return -1;
    GuestPft<char32_t> g = load_wpft(ctx, pf);
    const u64 limit = g.px.prec < 0 ? kNoLimit : 6 * static_cast<u64>(g.px.prec); // six bytes a character at most
    std::string s;
    bool terminated = true;
    if (const auto r = read_string(thread_of(ctx), pc, limit)) {
        s = *r;
        terminated = s.size() < limit;
    } else {
        refuse(ctx, std::format("_WPutstr: the text at {:#x} is not guest memory", pc));
    }
    ListArgs none(ctx, 0);
    s32 r;
    {
        GuestText<char32_t> text(ctx, none);
        GuestSink<char32_t> out(ctx, g.pfn, g.arg);
        r = wputstr(g.px, s, terminated, out, text);
        g.arg = out.arg();
    }
    store_wpft(ctx, pf, g);
    return r;
}

// void _WLitob(_WPft *px, wchar_t code)
void _WLitob_(PPUContext& ctx, u32 pf, u32 code)
{
    if (!range_ok(ctx, "_WLitob", pf, pft::kWideSize)) return;
    GuestPft<char32_t> g = load_wpft(ctx, pf);
    litob(g.px, static_cast<char>(code));
    store_wpft(ctx, pf, g);
}

// void _WLdtob(_WPft *px, wchar_t code)
void _WLdtob_(PPUContext& ctx, u32 pf, u32 code)
{
    if (!range_ok(ctx, "_WLdtob", pf, pft::kWideSize)) return;
    GuestPft<char32_t> g = load_wpft(ctx, pf);
    ldtob(g.px, static_cast<char>(code));
    store_wpft(ctx, pf, g);
}

// void _WGenld(_WPft *px, wchar_t code, const wchar_t *p, short nsig, short xexp)
void _WGenld_(PPUContext& ctx, u32 pf, u32 code, u32 p, s32 nsig, s32 xexp)
{
    if (!range_ok(ctx, "_WGenld", pf, pft::kWideSize)) return;
    GuestPft<char32_t> g = load_wpft(ctx, pf);
    genld(g.px, static_cast<char>(code), GPtr<char32_t>(&ctx, p), static_cast<s16>(nsig), static_cast<s16>(xexp));
    store_wpft(ctx, pf, g);
}

// ------------------------------------------------------------------------------------------------- SPU printf

// An SPU's local store as the library's accessors see it (sys_libc_spu.cpp, D-904): a word at a time, through lv2
// for an SPU thread (0 where lv2 refuses), from guest memory for a raw SPU (0 outside it); a char or short the byte
// or halfword of its aligned word, written back by a read, modify and write.
class SpuLs {
public:
    SpuLs(PPUContext& ctx, u32 id, bool raw) : ctx_(ctx), id_(id), raw_(raw) {}
    u32 id() const { return id_; }

    u32 word(u32 ls)
    {
        if (raw_) {
            const u32 ea = PS_SPU_RAW_BASE + 0x100000u * id_ + ls;
            if (u64{0x100000} * id_ + ls + 4 > 0x20000000ull || !guest_range(thread_of(ctx_), ea, 4)) return 0;
            return ps_read32(gb(ctx_), ea);
        }
        StackVar v(ctx_, gb(ctx_), 8);
        if (lv2::sys_spu_thread_read_ls(thread_of(ctx_), id_, ls, v.addr(), 4) != lv2::CELL_OK) return 0;
        return static_cast<u32>(ps_read64(gb(ctx_), v.addr()));
    }
    void put_word(u32 ls, u32 value)
    {
        if (raw_) {
            const u32 ea = PS_SPU_RAW_BASE + 0x100000u * id_ + ls;
            if (u64{0x100000} * id_ + ls + 4 > 0x20000000ull || !guest_range(thread_of(ctx_), ea, 4)) return;
            ps_write32(gb(ctx_), ea, value);
            return;
        }
        lv2::sys_spu_thread_write_ls(thread_of(ctx_), id_, ls, value, 4);
    }
    u8 byte(u32 ls)
    {
        u8 w[4];
        ps_write32(w, 0, word(ls & ~3u));
        return w[ls & 3];
    }
    s32 read_char(u32 ls) { return static_cast<s8>(byte(ls)); }
    s32 read_int(u32 ls) { return static_cast<s32>(word(ls)); }
    u32 read_uint(u32 ls) { return word(ls); }
    u64 read_llong(u32 ls)
    {
        const u32 hi = word(ls), lo = word(ls + 4);
        return (u64{hi} << 32) | lo;
    }
    void write_char(u32 ls, u32 v)
    {
        u8 w[4];
        ps_write32(w, 0, word(ls & ~3u));
        w[ls & 3] = static_cast<u8>(v);
        put_word(ls & ~3u, ps_read32(w, 0));
    }
    void write_short(u32 ls, u32 v)
    {
        u8 w[4];
        ps_write32(w, 0, word(ls & ~3u));
        ps_write16(w, 2 * ((ls >> 1) & 1), static_cast<u16>(v));
        put_word(ls & ~3u, ps_read32(w, 0));
    }
    void write_llong(u32 ls, u64 v)
    {
        put_word(ls, static_cast<u32>(v >> 32));
        put_word(ls + 4, static_cast<u32>(v));
    }

private:
    PPUContext& ctx_;
    u32 id_;
    bool raw_;
};

// __spu_thread_putfld: one conversion with its argument read from the store at ap (which moves a quadword for every
// conversion); always 0.
template <class P>
s32 spu_putfld(Pft<char, P>& px, u32& ls_text, SpuLs& ls, u32& ap, char code, P ac)
{
    const char q = static_cast<char>(px.qual);
    switch (code) {
    case 'S':
        px.qual = 'l';
        [[fallthrough]];
    case 's': {
        px.flags = static_cast<u16>(px.flags | kFlsText);
        const u32 p = ls.read_uint(ap);
        ls_text = p;
        px.n1 = 0;
        while ((px.prec < 0 || px.n1 < px.prec) && ls.read_char(p + static_cast<u32>(px.n1)) != 0) ++px.n1;
        break;
    }
    case 'd':
    case 'i':
        px.v = (q == 'q' || q == 'j') ? ls.read_llong(ap) : static_cast<u64>(static_cast<s64>(ls.read_int(ap)));
        if (q == 'h') px.v = static_cast<u64>(static_cast<s64>(static_cast<s16>(px.v)));
        else if (q == 'b') px.v = static_cast<u64>(static_cast<s64>(static_cast<s8>(px.v)));
        else if (q == 't' || q == 'z') px.v = static_cast<u64>(static_cast<s64>(static_cast<s32>(px.v)));
        if (static_cast<s64>(px.v) < 0) ac[px.n0++] = '-';
        else if (px.flags & kFpl) ac[px.n0++] = '+';
        else if (px.flags & kFsp) ac[px.n0++] = ' ';
        px.s = ac + px.n0;
        litob(px, code);
        break;
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G':
    case 'a':
    case 'A':
        px.v = ls.read_llong(ap);
        if (px.v >> 63) ac[px.n0++] = '-';
        else if (px.flags & kFpl) ac[px.n0++] = '+';
        else if (px.flags & kFsp) ac[px.n0++] = ' ';
        px.s = ac + px.n0;
        ldtob(px, code);
        break;
    case 'c':
        ac[px.n0++] = static_cast<char>(ls.read_int(ap));
        break;
    case 'p':
        px.v = ls.read_uint(ap);
        px.s = ac + px.n0;
        litob(px, 'x');
        break;
    case 'n': {
        const u32 p = ls.read_uint(ap);
        const u32 n = static_cast<u32>(px.nchar);
        if (q == 'b') ls.write_char(p, n);
        else if (q == 'h') ls.write_short(p, n);
        else if (q == 'j' || q == 'q') ls.write_llong(p, static_cast<u64>(static_cast<s64>(px.nchar)));
        else ls.put_word(p, n);
        break;
    }
    case 'o':
    case 'u':
    case 'x':
    case 'X':
        px.v = (q == 'q' || q == 'j') ? ls.read_llong(ap) : ls.read_uint(ap);
        if (q == 'h') px.v = static_cast<u16>(px.v);
        else if (q == 'b') px.v = static_cast<u8>(px.v);
        else if (q == 't' || q == 'z') px.v = static_cast<u32>(px.v);
        if ((px.flags & kFno) && px.v != 0 && (code == 'x' || code == 'X')) {
            ac[px.n0++] = '0';
            ac[px.n0++] = code;
        }
        px.s = ac + px.n0;
        litob(px, code);
        break;
    default: // '%' and any other character: itself (a NUL as '%')
        ac[px.n0++] = code ? code : '%';
        break;
    }
    ap += 16;
    return 0;
}

// __spu_thread_puttxt: _Puttxt, with %s's text read from the store into lsbuf (128 bytes) a piece at a time.
template <class P, class Sink>
bool spu_puttxt(Pft<char, P>& px, u32 ls_text, SpuLs& ls, P ac, P lsbuf, Sink& out)
{
    const s32 width = sub32(sub32(sub32(sub32(sub32(sub32(px.width, px.n0), px.nz0), px.n1), px.nz1), px.n2), px.nz2);
    if (!(px.flags & kFmi) && 0 < width && !out.pad(px, ' ', width)) return false;
    if (0 < px.n0 && !out.put(px, ac, px.n0)) return false;
    if (0 < px.nz0 && !out.pad(px, '0', px.nz0)) return false;
    if (px.flags & kFlsText) {
        u32 p = ls_text;
        s32 n = px.n1;
        if (!p) {
            const char* t = "(null)";
            for (s32 k = 0; k < 7; k++) lsbuf[k] = t[k];
            n = 6;
        } else {
            for (; static_cast<u32>(n) > 128; n -= 128, p += 128) {
                for (s32 k = 0; k < 128; k++) lsbuf[k] = static_cast<char>(ls.byte(p + static_cast<u32>(k)));
                if (!out.put(px, lsbuf, 128)) return false;
            }
            for (s32 k = 0; k < n; k++) lsbuf[k] = static_cast<char>(ls.byte(p + static_cast<u32>(k)));
        }
        if (0 < n && !out.put(px, lsbuf, n)) return false;
    } else {
        if (0 < px.n1 && !out.put(px, px.s, px.n1)) return false;
        if (0 < px.nz1 && !out.pad(px, '0', px.nz1)) return false;
        if (0 < px.n2 && !out.put(px, px.s + px.n1, px.n2)) return false;
    }
    if (0 < px.nz2 && !out.pad(px, '0', px.nz2)) return false;
    if ((px.flags & kFmi) && 0 < width && !out.pad(px, ' ', width)) return false;
    return true;
}

inline int spu_flag(s32 c)
{
    switch (c) {
    case ' ': return kFsp;
    case '+': return kFpl;
    case '-': return kFmi;
    case '#': return kFno;
    case '0': return kFze;
    default: return 0;
    }
}

// __spu_thread_printf: the format at the store's address in the first quadword of ls_args, its arguments after it.
// The count, or -1 when the sink stopped. A format that ends inside a conversion ends there (the library reads on
// past its NUL).
template <class P, class Sink>
s32 spu_printf(SpuLs& ls, u32 ls_args, Sink& out, P ac, P lsbuf, P one)
{
    Pft<char, P> px;
    px.nchar = 0;
    u32 fmt = ls.read_uint(ls_args);
    u32 ap = ls_args + 16;
    u32 ls_text = 0;
    for (;;) {
        s32 c = ls.read_char(fmt);
        if (c == 0) return px.nchar;
        ++fmt;
        if (c != '%') {
            one[0] = static_cast<char>(c);
            if (!out.put(px, one, 1)) return -1;
            continue;
        }
        px.n0 = px.nz0 = px.n1 = px.nz1 = px.n2 = px.nz2 = 0;
        px.flags = 0;
        for (int f; (f = spu_flag(c = ls.read_char(fmt))) != 0; ++fmt) px.flags = static_cast<u16>(px.flags | f);
        if (c == '*') {
            s32 w = ls.read_int(ap);
            ap += 16;
            if (w < 0) {
                w = sub32(0, w);
                px.flags = static_cast<u16>(px.flags | kFmi);
            }
            px.width = w;
            c = ls.read_char(++fmt);
        } else {
            s32 w = 0;
            for (; static_cast<u8>(c) >= '0' && static_cast<u8>(c) <= '9'; c = ls.read_char(++fmt))
                if (w != INT_MAX) w = mul10add(w, static_cast<u8>(c) - '0');
            px.width = w;
        }
        if (c == '.') {
            c = ls.read_char(++fmt);
            if (c == '*') {
                px.prec = ls.read_int(ap);
                ap += 16;
                c = ls.read_char(++fmt);
            } else {
                s32 p = 0;
                for (; static_cast<u8>(c) >= '0' && static_cast<u8>(c) <= '9'; c = ls.read_char(++fmt))
                    if (p != INT_MAX) p = mul10add(p, static_cast<u8>(c) - '0');
                px.prec = p;
            }
        } else {
            px.prec = -1;
        }
        px.qual = 0;
        if (c == 'h' || c == 'j' || c == 'l' || c == 't' || c == 'z' || c == 'L') {
            px.qual = static_cast<char>(c);
            const s32 d = ls.read_char(++fmt);
            if (c == 'h' && d == 'h') px.qual = 'b', ++fmt;
            else if (c == 'l' && d == 'l') px.qual = 'q', ++fmt;
            c = ls.read_char(fmt);
        }
        if (c == 0) return px.nchar;
        spu_putfld(px, ls_text, ls, ap, static_cast<char>(c), ac);
        if (!spu_puttxt(px, ls_text, ls, ac, lsbuf, out)) return -1;
        ++fmt;
    }
}

// int __spu_thread_printf(void *(*pfn)(void *, const char *, size_t), void *arg, sys_spu_thread_t spu, uint32_t
// ls_args), and the raw SPU's.
s32 spu_printf_guest(PPUContext& ctx, u32 pfn, u32 arg, u32 id, u32 ls_args, bool raw)
{
    StackVar ac(ctx, gb(ctx), 128);
    StackVar lsbuf(ctx, gb(ctx), 128);
    StackVar one(ctx, gb(ctx), 16);
    SpuLs ls(ctx, id, raw);
    GuestSink<char> out(ctx, pfn, arg);
    return spu_printf(ls, ls_args, out, GPtr<char>(&ctx, ac.addr()), GPtr<char>(&ctx, lsbuf.addr()),
        GPtr<char>(&ctx, one.addr()));
}
s32 __spu_thread_printf_(PPUContext& ctx, u32 pfn, u32 arg, u32 spu, u32 ls_args) { return spu_printf_guest(ctx, pfn, arg, spu, ls_args, false); }
s32 __raw_spu_printf_(PPUContext& ctx, u32 pfn, u32 arg, u32 id, u32 ls_args) { return spu_printf_guest(ctx, pfn, arg, id, ls_args, true); }

// int __spu_thread_putfld(_Pft *pft, sys_spu_thread_t spu, uint32_t *ap, char code, char *ac), and the raw SPU's.
s32 spu_putfld_guest(PPUContext& ctx, const char* fn, u32 pf, u32 id, u32 pap, s32 code, u32 ac, bool raw)
{
    if (!range_ok(ctx, fn, pf, pft::kSpuSize) || !range_ok(ctx, fn, pap, 4)) return -1;
    GuestPft<char> g = load_pft(ctx, pf, true);
    SpuLs ls(ctx, id, raw);
    u32 ap = ps_read32(gb(ctx), pap);
    const char c = static_cast<char>(code);
    if (c == 's' || c == 'S') g.spu = id;
    const s32 r = spu_putfld(g.px, g.ls_text, ls, ap, c, GPtr<char>(&ctx, ac));
    ps_write32(gb(ctx), pap, ap);
    store_pft(ctx, pf, g, true);
    return r;
}
s32 __spu_thread_putfld_(PPUContext& ctx, u32 pf, u32 spu, u32 pap, s32 code, u32 ac) { return spu_putfld_guest(ctx, "__spu_thread_putfld", pf, spu, pap, code, ac, false); }
s32 __raw_spu_putfld_(PPUContext& ctx, u32 pf, u32 id, u32 pap, s32 code, u32 ac) { return spu_putfld_guest(ctx, "__raw_spu_putfld", pf, id, pap, code, ac, true); }

// int __spu_thread_puttxt(_Pft *pft, const char *ac), and the raw SPU's: the SPU is the structure's (+84).
s32 spu_puttxt_guest(PPUContext& ctx, const char* fn, u32 pf, u32 ac, bool raw)
{
    if (!range_ok(ctx, fn, pf, pft::kSpuSize)) return -1;
    GuestPft<char> g = load_pft(ctx, pf, true);
    StackVar lsbuf(ctx, gb(ctx), 128);
    SpuLs ls(ctx, g.spu, raw);
    GuestSink<char> out(ctx, g.pfn, g.arg);
    const s32 r = spu_puttxt(g.px, g.ls_text, ls, GPtr<char>(&ctx, ac), GPtr<char>(&ctx, lsbuf.addr()), out) ? 0 : -1;
    g.arg = out.arg();
    store_pft(ctx, pf, g, true);
    return r;
}
s32 __spu_thread_puttxt_(PPUContext& ctx, u32 pf, u32 ac) { return spu_puttxt_guest(ctx, "__spu_thread_puttxt", pf, ac, false); }
s32 __raw_spu_puttxt_(PPUContext& ctx, u32 pf, u32 ac) { return spu_puttxt_guest(ctx, "__raw_spu_puttxt", pf, ac, true); }

// The SPU printf's text on the host (the sprintf forms and the TTY).
PrintResult<char> spu_text(PPUContext& ctx, u32 id, u32 ls_args, bool raw)
{
    SpuLs ls(ctx, id, raw);
    StringSink<char> out;
    char ac[128] = {}, lsbuf[136] = {}, one[2] = {};
    PrintResult<char> r;
    r.count = spu_printf<char*>(ls, ls_args, out, ac, lsbuf, one);
    r.text = std::move(out.text);
    return r;
}

// spu_thread_printf / raw_spu_printf: to the TTY's SPU channel (2), as the library's output function writes.
s32 spu_print(PPUContext& ctx, u32 id, u32 ls_args, bool raw)
{
    const PrintResult<char> r = spu_text(ctx, id, ls_args, raw);
    if (r.text.empty()) return r.count;
    lv2::PpuThread& t = thread_of(ctx);
    StackVar buf(ctx, gb(ctx), static_cast<u32>(r.text.size() + 8));
    std::memcpy(gb(ctx) + buf.addr(), r.text.data(), r.text.size());
    const u32 written = buf.addr() + static_cast<u32>(r.text.size());
    if (lv2::sys_tty_write(t, 2, buf.addr(), static_cast<u32>(r.text.size()), (written + 3) & ~3u) != lv2::CELL_OK) return -1;
    return r.count;
}

// spu_thread_sprintf: the text and a NUL (when the count is not negative).
s32 spu_sprint(PPUContext& ctx, const char* fn, u32 buf, u32 id, u32 ls_args, bool raw)
{
    const PrintResult<char> r = spu_text(ctx, id, ls_args, raw);
    const usize n = r.text.size() + (r.count >= 0 ? 1 : 0);
    if (!range_ok(ctx, fn, buf, n)) return -1;
    std::memcpy(gb(ctx) + buf, r.text.data(), r.text.size());
    if (r.count >= 0) gb(ctx)[buf + r.text.size()] = 0;
    return r.count;
}

// spu_thread_snprintf: at most size - 1 bytes and a NUL (nothing for size 0); the whole count.
s32 spu_snprint(PPUContext& ctx, const char* fn, u32 buf, u32 size, u32 id, u32 ls_args, bool raw)
{
    const PrintResult<char> r = spu_text(ctx, id, ls_args, raw);
    if (size) {
        const usize k = std::min<usize>(r.text.size(), size - 1);
        if (!range_ok(ctx, fn, buf, k + 1)) return -1;
        std::memcpy(gb(ctx) + buf, r.text.data(), k);
        gb(ctx)[buf + k] = 0;
    }
    return r.count;
}

s32 spu_thread_printf_(PPUContext& ctx, u32 spu, u32 ls_args) { return spu_print(ctx, spu, ls_args, false); }
s32 spu_thread_sprintf_(PPUContext& ctx, u32 buf, u32 spu, u32 ls_args) { return spu_sprint(ctx, "spu_thread_sprintf", buf, spu, ls_args, false); }
s32 spu_thread_snprintf_(PPUContext& ctx, u32 buf, u32 size, u32 spu, u32 ls_args)
{
    return spu_snprint(ctx, "spu_thread_snprintf", buf, size, spu, ls_args, false);
}
s32 raw_spu_printf_(PPUContext& ctx, u32 id, u32 ls_args) { return spu_print(ctx, id, ls_args, true); }
s32 spu_raw_sprintf_(PPUContext& ctx, u32 buf, u32 id, u32 ls_args) { return spu_sprint(ctx, "spu_raw_sprintf", buf, id, ls_args, true); }
s32 spu_raw_snprintf_(PPUContext& ctx, u32 buf, u32 size, u32 id, u32 ls_args)
{
    return spu_snprint(ctx, "spu_raw_snprintf", buf, size, id, ls_args, true);
}

} // namespace

namespace libc {

PrintResult<char> spu_thread_print_text(PPUContext& ctx, u32 spu, u32 ls_args) { return spu_text(ctx, spu, ls_args, false); }
s32 spu_thread_print_tty(PPUContext& ctx, u32 spu, u32 ls_args) { return spu_print(ctx, spu, ls_args, false); }

} // namespace libc

#define LIBC(name) ::pspocket::hle::Function{{}, #name, ::pspocket::recomp::nid_of(#name), &detail::thunk<&name##_>, false}

void register_sys_libc_format(Registry& r)
{
    for (const std::string_view lib : {std::string_view("sys_libc"), std::string_view("sys_libm")}) {
        r.add(lib, {
            LIBC(_Printf), LIBC(_WPrintf), LIBC(_Putfld), LIBC(_Puttxt), LIBC(_Putstr), LIBC(_Litob), LIBC(_Ldtob),
            LIBC(_Genld), LIBC(_WPutfld), LIBC(_WPuttxt), LIBC(_WPutstr), LIBC(_WLitob), LIBC(_WLdtob), LIBC(_WGenld),
            LIBC(__spu_thread_printf), LIBC(__spu_thread_putfld), LIBC(__spu_thread_puttxt), LIBC(__raw_spu_printf),
            LIBC(__raw_spu_putfld), LIBC(__raw_spu_puttxt), LIBC(spu_thread_printf), LIBC(spu_thread_sprintf),
            LIBC(spu_thread_snprintf), LIBC(raw_spu_printf), LIBC(spu_raw_sprintf), LIBC(spu_raw_snprintf),
        });
    }
}
#undef LIBC

} // namespace pspocket::hle
