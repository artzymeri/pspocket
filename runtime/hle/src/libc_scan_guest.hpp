#pragma once

// The guest side of sys_libc's parsing and scan engine (libc_scan.hpp, D-907): guest strings read character by
// character as the SDK's code reads them, the caller's input function, the variadic arguments a scanf stores through,
// and the conversions with their end pointers, errno and *perr as the SDK's functions leave them. Included by the
// sys_libc files that need them.

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "libc_guest.hpp"
#include "libc_scan.hpp"

namespace pspocket::hle::libc {

// A guest string's characters for the parsers: narrow bytes or 32-bit wide characters from `addr`, -1 where memory
// ends.
class GuestChars {
public:
    GuestChars(PPUContext& ctx, u32 addr, bool wide)
        : bytes_(thread_of(ctx), addr), words_(thread_of(ctx), addr), wide_(wide)
    {
    }
    s64 operator()(u64 i) { return wide_ ? words_.at(i) : bytes_.at(i); }
    u32 unit() const { return wide_ ? 4 : 1; }

private:
    GuestBytes bytes_;
    GuestWords words_;
    bool wide_;
};

// strtol and its kin: _Stolx/_Stoulx (32 bits) and _Stollx/_Stoullx (64) on the guest string at s; *endp (when given)
// and errno, and *perr (when given: 0, or 1 on a range error) as the SDK's functions set them.
inline u64 guest_strtoint(PPUContext& ctx, const char* fn, u32 s, u32 endp, s32 base, u32 bits, bool is_signed, u32 perr,
    bool wide)
{
    if (perr && range_ok(ctx, fn, perr, 4)) ps_write32(guest_base(ctx), perr, 0);
    GuestChars at(ctx, s, wide);
    const IntResult r = is_signed ? sdk_stolx(at, 0, base, bits) : sdk_stoulx(at, 0, base, bits);
    if (r.range) {
        set_errno(ctx, kERANGE);
        if (perr && range_ok(ctx, fn, perr, 4)) ps_write32(guest_base(ctx), perr, 1);
    }
    if (endp && range_ok(ctx, fn, endp, 4)) ps_write32(guest_base(ctx), endp, s + at.unit() * static_cast<u32>(r.end));
    return r.value;
}

// strtod and its kin: _Stodx (_Stoldx) and _Stofx on the guest string at s, scaled by 10^pten.
inline double guest_strtod(PPUContext& ctx, const char* fn, u32 s, u32 endp, s32 pten, u32 perr, bool wide, bool single)
{
    if (perr && range_ok(ctx, fn, perr, 4)) ps_write32(guest_base(ctx), perr, 0);
    GuestChars at(ctx, s, wide);
    double value = 0;
    u64 end = 0;
    bool range = false;
    if (single) {
        const ParseResult<float> r = sdk_stofx(at, 0, pten);
        value = static_cast<double>(r.value);
        end = r.end;
        range = r.range;
    } else {
        const ParseResult<double> r = sdk_stodx(at, 0, pten);
        value = r.value;
        end = r.end;
        range = r.range;
    }
    if (range) {
        set_errno(ctx, kERANGE);
        if (perr && range_ok(ctx, fn, perr, 4)) ps_write32(guest_base(ctx), perr, ps_read32(guest_base(ctx), perr) | 1);
    }
    if (endp && range_ok(ctx, fn, endp, 4)) ps_write32(guest_base(ctx), endp, s + at.unit() * static_cast<u32>(end));
    return value;
}

// ------------------------------------------------------------------------------------------------- scan input, output

// The caller's int pfn(void *arg, int ch, int getfl), called as the SDK's engine calls it.
class GuestScanIo final : public ScanIo {
public:
    GuestScanIo(PPUContext& ctx, u32 pfn, u32 arg) : ctx_(ctx), pfn_(pfn), arg_(arg) {}
    s32 get() override { return static_cast<s32>(call_guest(ctx_, pfn_, {arg_, 0, 1})); }
    void unget(s32 c) override { call_guest(ctx_, pfn_, {arg_, static_cast<u64>(static_cast<s64>(c)), 0}); }

private:
    PPUContext& ctx_;
    u32 pfn_, arg_;
};

// sscanf's and swscanf's input: the guest string's characters (bytes, or 32-bit wide characters), EOF at its NUL,
// where it stays (the SDK's string reader does not move past it); a push-back steps back one character.
class GuestTextIo final : public ScanIo {
public:
    GuestTextIo(PPUContext& ctx, u32 addr, bool wide) : at_(ctx, addr, wide) {}
    s32 get() override
    {
        const s64 c = at_(pos_);
        if (c <= 0) return -1;
        pos_++;
        return static_cast<s32>(c);
    }
    void unget(s32) override
    {
        if (pos_) pos_--;
    }

private:
    GuestChars at_;
    u64 pos_ = 0;
};

// Where a scanf stores: pointers from the call's variadic arguments (or its va_list) into guest memory; errno for a
// conversion's range error.
class GuestScanArgs final : public ScanArgs {
public:
    GuestScanArgs(PPUContext& ctx, VarArgs& args) : ctx_(ctx), args_(&args) {}
    // va_arg on a guest va_list kept in an _Sft: the pointer in the low word of the slot at ap, ap moved by 8.
    GuestScanArgs(PPUContext& ctx, u32 ap) : ctx_(ctx), ap_(ap) {}

    u32 next_pointer() override
    {
        if (args_) return static_cast<u32>(args_->next());
        const u32 slot = ap_;
        ap_ += 8;
        if (!guest_range(thread_of(ctx_), slot, 8)) {
            problems.push_back(std::format("the va_list slot {:#x} is not memory", slot));
            return 0;
        }
        return ps_read32(guest_base(ctx_), slot + 4);
    }
    bool put(u32 addr, const u8* data, usize size) override
    {
        if (!guest_range(thread_of(ctx_), addr, size)) return false;
        std::memcpy(guest_base(ctx_) + addr, data, size);
        return true;
    }
    void range_error() override { set_errno(ctx_, kERANGE); }
    u32 ap() const { return ap_; }

private:
    PPUContext& ctx_;
    VarArgs* args_ = nullptr;
    u32 ap_ = 0;
};

// A guest format for the engine: its characters to the NUL and two NULs after them (a '%' the format ends in reads one
// past its end); nullopt when it runs out of guest memory first.
template <class C>
std::optional<std::basic_string<C>> scan_format(PPUContext& ctx, u32 fmt)
{
    std::optional<std::basic_string<C>> f;
    if constexpr (std::is_same_v<C, char>) f = read_string(thread_of(ctx), fmt);
    else f = read_wstring(thread_of(ctx), fmt, kMaxString / 4);
    if (f) f->push_back(C{0});
    return f;
}

// A scanf of the guest format at `fmt` on `io`, stored through `out`: the SDK's _Scanf (_WScanf).
template <class C>
s32 guest_scanf(PPUContext& ctx, const char* fn, u32 fmt, ScanIo& io, ScanArgs& out)
{
    const auto f = scan_format<C>(ctx, fmt);
    if (!f) {
        refuse(ctx, std::format("{}: the format at {:#x} runs out of guest memory", fn, fmt));
        return -1;
    }
    const s32 r = sdk_scanf<C>(io, out, f->c_str());
    for (const std::string& p : out.problems) refuse(ctx, std::format("{}: {}", fn, p));
    return r;
}

} // namespace pspocket::hle::libc
