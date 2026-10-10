// sys_libc part 6 (D-904): the rest of Sony's additions to the C library, under sys_libc and sys_libm, as the SDK's
// library a game links statically has them (Assassin's Creed's copy was read: no firmware module has the C library).
// - An SPU's local store from the PPU. spu_thread_read_* / spu_thread_write_* go through lv2's
//   sys_spu_thread_read_ls / write_ls a word at a time, as the SDK's do: a char or short is the byte or halfword of
//   its aligned word (written back as a read, modify and write of that word); an int, long, pointer or float is the
//   word at the address as given (an address lv2 refuses leaves the value 0, where the SDK's reads its stack); a long
//   long or double is the word at the address and the one after it, high first. A write answers lv2's result (the
//   second word's for a 64-bit value). spu_thread_read_mem reads a byte at a time; spu_thread_write_mem writes whole
//   words while more than four bytes remain, then merges the last one to four bytes into the word there (reading and
//   writing it back, even for none).
//   raw_spu_read_* / raw_spu_write_* are the same on a raw SPU's local store, which is guest memory at
//   0xE0000000 + 0x100000 * id (D-528): the SDK's executables seen link no raw SPU forms, so they follow the SPU
//   threads' ones word for word with the word read and written directly; an address outside committed memory reads
//   0 and writes nothing (answering CELL_EFAULT).
// - _get_fd and _put_fd: Sony's table of 256 descriptor slots (0..2 hold 0..2, the rest free, -1): _get_fd(value,
//   extra) takes the first free slot for value and an extra word and answers its index (-1 when none is free);
//   _put_fd(index) frees a slot below 256. The library's own open and close never use it.
// - _Getmem(size): Dinkumware's (Plauger's) system-dependent call for raw memory, from the main-area allocator,
//   never given back; NULL when there is none. The SDK's copies seen have none of their own (their malloc is
//   dlmalloc, D-502).
// - No executable seen links _Atrealloc, _Once_ctor, _Once_dtor or the _d2rv, _rv2d, _Fd2rv, _Frv2d, _Ld2rv, _Lrv2d
//   conversions, and nothing describes them: they log the call and answer an error (D-510's way with _Eadd and kin).

#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <mutex>

#include "libc_guest.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/spu/context.h"

namespace pspocket::hle {
namespace {

using namespace libc;

u8* gb(PPUContext& ctx) { return guest_base(ctx); }

// ------------------------------------------------------------------------------------------------- SPU threads

struct Word {
    s32 result;
    u32 value;
};

// One word of an SPU thread's local store through lv2 (4 bytes at `ls`; lv2 refuses a misaligned or out-of-range one).
Word thread_read(PPUContext& ctx, u32 spu, u32 ls)
{
    lv2::PpuThread& t = thread_of(ctx);
    StackVar v(ctx, gb(ctx), 8);
    const s32 r = lv2::sys_spu_thread_read_ls(t, spu, ls, v.addr(), 4);
    return {r, r == lv2::CELL_OK ? static_cast<u32>(ps_read64(gb(ctx), v.addr())) : 0u};
}

s32 thread_write(PPUContext& ctx, u32 spu, u32 ls, u64 value)
{
    return lv2::sys_spu_thread_write_ls(thread_of(ctx), spu, ls, value, 4);
}

// ------------------------------------------------------------------------------------------------- raw SPUs

u32 raw_ls(u32 id, u32 ls) { return PS_SPU_RAW_BASE + 0x100000u * id + ls; }

Word raw_read(PPUContext& ctx, u32 id, u32 ls)
{
    const u32 ea = raw_ls(id, ls);
    if (u64{0x100000} * id + ls + 4 > 0x20000000ull || !guest_range(thread_of(ctx), ea, 4))
        return {static_cast<s32>(lv2::CELL_EFAULT), 0};
    return {lv2::CELL_OK, ps_read32(gb(ctx), ea)};
}

s32 raw_write(PPUContext& ctx, u32 id, u32 ls, u64 value)
{
    const u32 ea = raw_ls(id, ls);
    if (u64{0x100000} * id + ls + 4 > 0x20000000ull || !guest_range(thread_of(ctx), ea, 4))
        return static_cast<s32>(lv2::CELL_EFAULT);
    ps_write32(gb(ctx), ea, static_cast<u32>(value));
    return lv2::CELL_OK;
}

// The accessors over one of the two word functions.
template <Word (*Read)(PPUContext&, u32, u32), s32 (*Write)(PPUContext&, u32, u32, u64)>
struct Ls {
    static u32 byte_of(PPUContext& ctx, u32 spu, u32 ls)
    {
        u8 w[4];
        ps_write32(w, 0, Read(ctx, spu, ls & ~3u).value);
        return w[ls & 3];
    }
    static u32 half_of(PPUContext& ctx, u32 spu, u32 ls)
    {
        u8 w[4];
        ps_write32(w, 0, Read(ctx, spu, ls & ~3u).value);
        return ps_read16(w, 2 * ((ls >> 1) & 1));
    }
    static u64 dword_of(PPUContext& ctx, u32 spu, u32 ls)
    {
        const u32 hi = Read(ctx, spu, ls).value, lo = Read(ctx, spu, ls + 4).value;
        return (u64{hi} << 32) | lo;
    }

    static s32 read_char(PPUContext& ctx, u32 spu, u32 ls) { return static_cast<s8>(byte_of(ctx, spu, ls)); }
    static s32 read_short(PPUContext& ctx, u32 spu, u32 ls) { return static_cast<s16>(half_of(ctx, spu, ls)); }
    static s32 read_int(PPUContext& ctx, u32 spu, u32 ls) { return static_cast<s32>(Read(ctx, spu, ls).value); }
    static u64 read_llong(PPUContext& ctx, u32 spu, u32 ls) { return dword_of(ctx, spu, ls); }
    static u32 read_uchar(PPUContext& ctx, u32 spu, u32 ls) { return byte_of(ctx, spu, ls); }
    static u32 read_ushort(PPUContext& ctx, u32 spu, u32 ls) { return half_of(ctx, spu, ls); }
    static u32 read_uint(PPUContext& ctx, u32 spu, u32 ls) { return Read(ctx, spu, ls).value; }
    static float read_float(PPUContext& ctx, u32 spu, u32 ls)
    {
        return std::bit_cast<float>(Read(ctx, spu, ls).value);
    }
    static double read_double(PPUContext& ctx, u32 spu, u32 ls) { return std::bit_cast<double>(dword_of(ctx, spu, ls)); }

    // void read_mem(void *dst, ...) as the SDK has it: (spu, ls, dst, size), a byte at a time; r3 is left as the last
    // read's result (the id when there is nothing to read).
    static u64 read_mem(PPUContext& ctx, u64 spu, u32 ls, u32 dst, u32 size)
    {
        if (!size) return spu;
        if (!range_ok(ctx, "spu_thread_read_mem", dst, size)) return static_cast<u32>(lv2::CELL_EFAULT);
        s32 r = lv2::CELL_OK;
        for (u32 i = 0; i < size; i++) {
            const Word w = Read(ctx, static_cast<u32>(spu), (ls + i) & ~3u);
            r = w.result;
            u8 b[4];
            ps_write32(b, 0, w.value);
            gb(ctx)[dst + i] = b[(ls + i) & 3];
        }
        return static_cast<u64>(static_cast<s64>(r));
    }

    static s32 write_byte(PPUContext& ctx, u32 spu, u32 ls, u32 value)
    {
        u8 w[4];
        ps_write32(w, 0, Read(ctx, spu, ls & ~3u).value);
        w[ls & 3] = static_cast<u8>(value);
        return Write(ctx, spu, ls & ~3u, ps_read32(w, 0));
    }
    static s32 write_half(PPUContext& ctx, u32 spu, u32 ls, u32 value)
    {
        u8 w[4];
        ps_write32(w, 0, Read(ctx, spu, ls & ~3u).value);
        ps_write16(w, 2 * ((ls >> 1) & 1), static_cast<u16>(value));
        return Write(ctx, spu, ls & ~3u, ps_read32(w, 0));
    }
    static s32 write_dword(PPUContext& ctx, u32 spu, u32 ls, u64 value)
    {
        Write(ctx, spu, ls, value >> 32);
        return Write(ctx, spu, ls + 4, static_cast<u32>(value));
    }

    static s32 write_char(PPUContext& ctx, u32 spu, u32 ls, u32 v) { return write_byte(ctx, spu, ls, v); }
    static s32 write_short(PPUContext& ctx, u32 spu, u32 ls, u32 v) { return write_half(ctx, spu, ls, v); }
    static s32 write_int(PPUContext& ctx, u32 spu, u32 ls, u32 v) { return Write(ctx, spu, ls, v); }
    static s32 write_word64(PPUContext& ctx, u32 spu, u32 ls, u64 v) { return Write(ctx, spu, ls, v); }
    static s32 write_llong(PPUContext& ctx, u32 spu, u32 ls, u64 v) { return write_dword(ctx, spu, ls, v); }
    static s32 write_float(PPUContext& ctx, u32 spu, u32 ls, float v)
    {
        return Write(ctx, spu, ls, std::bit_cast<u32>(v));
    }
    static s32 write_double(PPUContext& ctx, u32 spu, u32 ls, double v)
    {
        return write_dword(ctx, spu, ls, std::bit_cast<u64>(v));
    }
    // (spu, ls, src, size): whole words while more than four bytes remain, then the last one to four bytes merged
    // into the word at that address.
    static s32 write_mem(PPUContext& ctx, u32 spu, u32 ls, u32 src, u32 size)
    {
        if (!range_ok(ctx, "spu_thread_write_mem", src, size)) return static_cast<s32>(lv2::CELL_EFAULT);
        u8* b = gb(ctx);
        while (size > 4) {
            Write(ctx, spu, ls, ps_read32(b, src));
            ls += 4;
            src += 4;
            size -= 4;
        }
        u8 w[4];
        ps_write32(w, 0, Read(ctx, spu, ls).value);
        std::memcpy(w, b + src, size);
        return Write(ctx, spu, ls, ps_read32(w, 0));
    }
};

using Thread = Ls<&thread_read, &thread_write>;
using Raw = Ls<&raw_read, &raw_write>;

// ------------------------------------------------------------------------------------------------- _get_fd, _put_fd

struct FdTable {
    std::mutex mutex;
    std::array<s32, 256> value{};
    std::array<u32, 256> extra{};
    FdTable()
    {
        value.fill(-1);
        for (s32 i = 0; i < 3; i++) value[static_cast<usize>(i)] = i;
    }
};

FdTable& fds() { return HleProcess::current()->state<FdTable>(); }

s32 _get_fd_(PPUContext&, s32 value, u32 extra)
{
    FdTable& t = fds();
    std::lock_guard l(t.mutex);
    for (usize i = 0; i < t.value.size(); i++) {
        if (t.value[i] != -1) continue;
        t.value[i] = value;
        t.extra[i] = extra;
        return static_cast<s32>(i);
    }
    return -1;
}

void _put_fd_(PPUContext&, u32 index)
{
    FdTable& t = fds();
    std::lock_guard l(t.mutex);
    if (index < t.value.size()) t.value[index] = -1;
}

// ------------------------------------------------------------------------------------------------- _Getmem

u32 _Getmem_(PPUContext&, u32 size)
{
    if (!size) return 0;
    return HleProcess::current()->main_memory().alloc((size + 15) & ~15u, 16);
}

// ------------------------------------------------------------------------------------------------- unknown

template <const char* Name>
void unknown(PPUContext& ctx, u8* base)
{
    log_call_site(ctx, base,
        std::format("{}: no executable seen links it and nothing describes it; not done (D-904)", Name));
    ctx.gpr[3] = ~u64{0};
    ctx.fpr[1] = std::bit_cast<double>(0x7FF8000000000000ull);
}

constexpr char kAtrealloc[] = "_Atrealloc", kOnceCtor[] = "_Once_ctor", kOnceDtor[] = "_Once_dtor", kD2rv[] = "_d2rv",
               kRv2d[] = "_rv2d", kFd2rv[] = "_Fd2rv", kFrv2d[] = "_Frv2d", kLd2rv[] = "_Ld2rv", kLrv2d[] = "_Lrv2d";

} // namespace

#define F(name, body) ::pspocket::hle::Function{{}, name, ::pspocket::recomp::nid_of(name), &detail::thunk<body>, false}
#define RAWFN(name, body) ::pspocket::hle::Function{{}, name, ::pspocket::recomp::nid_of(name), body, false}

void register_sys_libc_spu(Registry& r)
{
    for (const std::string_view lib : {std::string_view("sys_libc"), std::string_view("sys_libm")}) {
        r.add(lib, {
            F("spu_thread_read_char", &Thread::read_char),
            F("spu_thread_read_short", &Thread::read_short),
            F("spu_thread_read_int", &Thread::read_int),
            F("spu_thread_read_long", &Thread::read_int),
            F("spu_thread_read_llong", &Thread::read_llong),
            F("spu_thread_read_uchar", &Thread::read_uchar),
            F("spu_thread_read_ushort", &Thread::read_ushort),
            F("spu_thread_read_uint", &Thread::read_uint),
            F("spu_thread_read_ulong", &Thread::read_uint),
            F("spu_thread_read_ullong", &Thread::read_llong),
            F("spu_thread_read_float", &Thread::read_float),
            F("spu_thread_read_double", &Thread::read_double),
            F("spu_thread_read_ldouble", &Thread::read_double),
            F("spu_thread_read_ptr", &Thread::read_uint),
            F("spu_thread_read_mem", &Thread::read_mem),
            F("spu_thread_write_char", &Thread::write_char),
            F("spu_thread_write_short", &Thread::write_short),
            F("spu_thread_write_int", &Thread::write_int),
            F("spu_thread_write_long", &Thread::write_int),
            F("spu_thread_write_llong", &Thread::write_llong),
            F("spu_thread_write_uchar", &Thread::write_char),
            F("spu_thread_write_ushort", &Thread::write_short),
            F("spu_thread_write_uint", &Thread::write_word64),
            F("spu_thread_write_ulong", &Thread::write_word64),
            F("spu_thread_write_ullong", &Thread::write_llong),
            F("spu_thread_write_float", &Thread::write_float),
            F("spu_thread_write_double", &Thread::write_double),
            F("spu_thread_write_ldouble", &Thread::write_double),
            F("spu_thread_write_ptr", &Thread::write_word64),
            F("spu_thread_write_mem", &Thread::write_mem),
            F("raw_spu_read_char", &Raw::read_char),
            F("raw_spu_read_short", &Raw::read_short),
            F("raw_spu_read_int", &Raw::read_int),
            F("raw_spu_read_long", &Raw::read_int),
            F("raw_spu_read_llong", &Raw::read_llong),
            F("raw_spu_read_uchar", &Raw::read_uchar),
            F("raw_spu_read_ushort", &Raw::read_ushort),
            F("raw_spu_read_uint", &Raw::read_uint),
            F("raw_spu_read_ulong", &Raw::read_uint),
            F("raw_spu_read_ullong", &Raw::read_llong),
            F("raw_spu_read_float", &Raw::read_float),
            F("raw_spu_read_double", &Raw::read_double),
            F("raw_spu_read_ldouble", &Raw::read_double),
            F("raw_spu_read_ptr", &Raw::read_uint),
            F("raw_spu_read_mem", &Raw::read_mem),
            F("raw_spu_write_char", &Raw::write_char),
            F("raw_spu_write_short", &Raw::write_short),
            F("raw_spu_write_int", &Raw::write_int),
            F("raw_spu_write_long", &Raw::write_int),
            F("raw_spu_write_llong", &Raw::write_llong),
            F("raw_spu_write_uchar", &Raw::write_char),
            F("raw_spu_write_ushort", &Raw::write_short),
            F("raw_spu_write_uint", &Raw::write_word64),
            F("raw_spu_write_ulong", &Raw::write_word64),
            F("raw_spu_write_ullong", &Raw::write_llong),
            F("raw_spu_write_float", &Raw::write_float),
            F("raw_spu_write_double", &Raw::write_double),
            F("raw_spu_write_ldouble", &Raw::write_double),
            F("raw_spu_write_ptr", &Raw::write_word64),
            F("raw_spu_write_mem", &Raw::write_mem),
            F("_get_fd", &_get_fd_),
            F("_put_fd", &_put_fd_),
            F("_Getmem", &_Getmem_),
            RAWFN("_Atrealloc", &unknown<kAtrealloc>),
            RAWFN("_Once_ctor", &unknown<kOnceCtor>),
            RAWFN("_Once_dtor", &unknown<kOnceDtor>),
            RAWFN("_d2rv", &unknown<kD2rv>),
            RAWFN("_rv2d", &unknown<kRv2d>),
            RAWFN("_Fd2rv", &unknown<kFd2rv>),
            RAWFN("_Frv2d", &unknown<kFrv2d>),
            RAWFN("_Ld2rv", &unknown<kLd2rv>),
            RAWFN("_Lrv2d", &unknown<kLrv2d>),
        });
    }
}
#undef F
#undef RAWFN

} // namespace pspocket::hle
