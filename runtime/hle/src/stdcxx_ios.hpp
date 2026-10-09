#pragma once

// sys_libstdcxx, the streams (D-560): ios_base (and failure, Init, _Winit), basic_ios, basic_streambuf, basic_filebuf
// and _Fiopen over sys_libc's stdio, the istream/ostream/iostream destructors, strstreambuf and its streams, the
// stream buffer iterators and the manipulators. Included by stdcxx_streams.cpp only.
//
// Layouts (the SDK's; E is the element's size, 1 for char and 4 for wchar_t):
// - ios_base (40 bytes): vptr, size_t _Stdstr (+4), _Mystate (+8), _Except (+12), _Fmtfl (+16), _Prec (+20), _Wide
//   (+24), _Iosarray *_Arr (+28), _Fnarray *_Calls (+32), locale *_Ploc (+36). _Iosarray {next, index, long, void *},
//   _Fnarray {next, index, callback}, each from operator new.
// - basic_ios : ios_base {basic_streambuf *_Mystrbuf (+40), basic_ostream *_Tiestr (+44), _Elem _Fillch (+48)}: 52.
// - basic_streambuf (60 bytes): vptr, _Mutex _Mylock (+4), _Gfirst, _Pfirst (+8, +12), _IGfirst, _IPfirst (+16, +20),
//   _Gnext, _Pnext (+24, +28), _IGnext, _IPnext (+32, +36), _Gend, _Pend (+40, +44), _IGend, _IPend (+48, +52),
//   locale *_Plocale (+56). The SDK's keeps end pointers, not counts: egptr() is *_IGend, epptr() *_IPend.
// - basic_filebuf : basic_streambuf {codecvt *_Pcvt (+60), _Elem _Mychar (+64), bool _Wrotesome, _Mbstatet _State
//   (8 bytes), bool _Closef, FILE *_Myfile}: char 84 bytes (+65, +68, +76, +80), wchar_t 88 (+68, +72, +80, +84).
// - strstreambuf : basic_streambuf<char> {char *_Pendsave (+60), *_Seekhigh (+64), int _Alsize (+68), int _Strmode
//   (+72), void *(*_Palloc)(size_t) (+76), void (*_Pfree)(void *) (+80)}: 84 bytes.
// - basic_istream {vptr, streamsize _Chcount, basic_ios (virtual)}: 60; basic_ostream {vptr, basic_ios}: 56;
//   basic_iostream {istream's vptr and _Chcount, ostream's vptr (+8), basic_ios (+12)}: 64; istrstream {istream's,
//   strstreambuf (+8), basic_ios (+92)}: 144; ostrstream {vptr, strstreambuf (+4), basic_ios (+88)}: 140; strstream
//   {iostream's, strstreambuf (+12), basic_ios (+96)}: 148.
// - pos_type (fpos<_Mbstatet>, 24 bytes): streamoff _Myoff, fpos_t _Fpos {long _Off, _Mbstatet _Wstate} (+4),
//   _Mbstatet _Mystate (+16); passed by value in three doublewords.
// - istreambuf_iterator {streambuf *_Strbuf, bool _Got (+4), _Elem _Val (+5 for char, +8 for wchar_t)};
//   ostreambuf_iterator {bool _Failed, streambuf *_Strbuf (+4)}; _Smanip<int> {void (*_Pfun)(ios_base&, int), int}.

#include "stdcxx_locale.hpp"

namespace pspocket::hle::stdcxx {

inline constexpr u32 kIosStdstr = 4, kIosState = 8, kIosExcept = 12, kIosFmtfl = 16, kIosPrec = 20, kIosWide = 24,
                     kIosArr = 28, kIosCalls = 32, kIosPloc = 36, kIosBaseBytes = 40;
inline constexpr u32 kIosSb = 40, kIosTie = 44, kIosFill = 48, kBasicIosBytes = 52;
inline constexpr u32 kGoodbit = 0, kEofbit = 1, kFailbit = 2, kBadbit = 4, kStatmask = 0x17;
inline constexpr u32 kSkipws = 0x1, kUnitbuf = 0x2, kDec = 0x200, kOct = 0x400, kHex = 0x800, kBasefield = 0xE00,
                     kFmtmask = 0xFFFF;
inline constexpr u32 kIn = 0x1, kOut = 0x2, kAte = 0x4, kApp = 0x8, kTrunc = 0x10, kBinary = 0x20, kNocreate = 0x40,
                     kNoreplace = 0x80;
inline constexpr s32 kEof = -1;
inline constexpr s32 kBadoff = -1;

inline constexpr u32 kSbLock = 4, kSbGfirst = 8, kSbPfirst = 12, kSbIGfirst = 16, kSbIPfirst = 20, kSbGnext = 24,
                     kSbPnext = 28, kSbIGnext = 32, kSbIPnext = 36, kSbGend = 40, kSbPend = 44, kSbIGend = 48,
                     kSbIPend = 52, kSbPloc = 56, kSbBytes = 60;
inline constexpr u32 kSsPendsave = 60, kSsSeekhigh = 64, kSsAlsize = 68, kSsStrmode = 72, kSsPalloc = 76,
                     kSsPfree = 80, kSsBytes = 84;
inline constexpr u32 kSsAllocated = 1, kSsConstant = 2, kSsDynamic = 4, kSsFrozen = 8;
inline constexpr s32 kSsMinsize = 32, kSsAlsizeDefault = 512;

template <u32 E>
struct FbLayout {
    static constexpr u32 kPcvt = 60, kMychar = 64, kWrotesome = E == 1 ? 65 : 68, kState = E == 1 ? 68 : 72,
                         kClosef = E == 1 ? 76 : 80, kFile = E == 1 ? 80 : 84, kBytes = E == 1 ? 84 : 88;
};

// Mangled names by element.
template <u32 E>
struct Names {
    static constexpr std::string_view ios = E == 1 ? "St9basic_iosIcSt11char_traitsIcEE" : "St9basic_iosIwSt11char_traitsIwEE";
    static constexpr std::string_view sb = E == 1 ? "St15basic_streambufIcSt11char_traitsIcEE"
                                                  : "St15basic_streambufIwSt11char_traitsIwEE";
    static constexpr std::string_view fb = E == 1 ? "St13basic_filebufIcSt11char_traitsIcEE"
                                                  : "St13basic_filebufIwSt11char_traitsIwEE";
    static constexpr std::string_view is = E == 1 ? "Si" : "St13basic_istreamIwSt11char_traitsIwEE";
    static constexpr std::string_view os = E == 1 ? "So" : "St13basic_ostreamIwSt11char_traitsIwEE";
    static constexpr std::string_view ctype = E == 1 ? kCtypeC : kCtypeW;
    static constexpr std::string_view codecvt = E == 1 ? kCodecvtC : kCodecvtW;
};

// int_type of an element (char_traits<char>::to_int_type is the unsigned char's value).
template <u32 E>
inline s32 to_int(u32 ch)
{
    if constexpr (E == 1) return static_cast<s32>(ch & 0xFF);
    else return static_cast<s32>(ch);
}

// ------------------------------------------------------------------------------------------------- pos_type

// A pos_type written at `out`: {_Myoff, _Fpos {_Off, state}, _Mystate}.
inline void pos_write(const G& g, u32 out, s32 myoff, s32 fpos_off, u64 fpos_state, u64 mystate)
{
    g.w32(out, static_cast<u32>(myoff));
    g.w32(out + 4, static_cast<u32>(fpos_off));
    g.w32(out + 8, static_cast<u32>(fpos_state >> 32));
    g.w32(out + 12, static_cast<u32>(fpos_state));
    g.w32(out + 16, static_cast<u32>(mystate >> 32));
    g.w32(out + 20, static_cast<u32>(mystate));
}

// pos_type(streamoff): the offset, a zero file position and state.
inline void pos_off(const G& g, u32 out, s32 off) { pos_write(g, out, off, 0, 0, 0); }

// A pos_type argument from its three doublewords.
struct Pos {
    s32 myoff;
    s32 fpos_off;
    u64 fpos_state;
    u64 mystate;
    Pos(u64 d0, u64 d1, u64 d2)
        : myoff(static_cast<s32>(d0 >> 32)), fpos_off(static_cast<s32>(d0)), fpos_state(d1), mystate(d2)
    {
    }
    // (streamoff)pos: _Myoff + _FPOSOFF(_Fpos)
    s32 offset() const { return static_cast<s32>(static_cast<u32>(myoff) + static_cast<u32>(fpos_off)); }
};

// ------------------------------------------------------------------------------------------------- streambuf

// A basic_streambuf in guest memory, through its indirect pointers (eback() is *_IGfirst and so on).
template <u32 E>
class Sb {
public:
    Sb(const G& g, u32 self) : g_(g), self_(self) {}

    u32 self() const { return self_; }
    u32 ind(u32 field) const { return g_.r32(self_ + field); }
    u32 eback() const { return g_.r32(ind(kSbIGfirst)); }
    u32 gptr() const { return g_.r32(ind(kSbIGnext)); }
    u32 egptr() const { return g_.r32(ind(kSbIGend)); }
    u32 pbase() const { return g_.r32(ind(kSbIPfirst)); }
    u32 pptr() const { return g_.r32(ind(kSbIPnext)); }
    u32 epptr() const { return g_.r32(ind(kSbIPend)); }
    void setg(u32 first, u32 next, u32 last) const
    {
        g_.w32(ind(kSbIGfirst), first);
        g_.w32(ind(kSbIGnext), next);
        g_.w32(ind(kSbIGend), last);
    }
    void setp(u32 first, u32 last) const { setp(first, first, last); }
    void setp(u32 first, u32 next, u32 last) const
    {
        g_.w32(ind(kSbIPfirst), first);
        g_.w32(ind(kSbIPnext), next);
        g_.w32(ind(kSbIPend), last);
    }
    void gbump(s32 n) const { g_.w32(ind(kSbIGnext), gptr() + static_cast<u32>(n * static_cast<s32>(E))); }
    void pbump(s32 n) const { g_.w32(ind(kSbIPnext), pptr() + static_cast<u32>(n * static_cast<s32>(E))); }
    s32 gnavail() const { return gptr() ? static_cast<s32>(egptr() - gptr()) / static_cast<s32>(E) : 0; }
    s32 pnavail() const { return pptr() ? static_cast<s32>(epptr() - pptr()) / static_cast<s32>(E) : 0; }
    u32 el(u32 a) const { return g_.template rel<E>(a); }

    // _Init(): the buffer pointers are the object's own, all null.
    void init() const
    {
        g_.w32(self_ + kSbIGfirst, self_ + kSbGfirst);
        g_.w32(self_ + kSbIPfirst, self_ + kSbPfirst);
        g_.w32(self_ + kSbIGnext, self_ + kSbGnext);
        g_.w32(self_ + kSbIPnext, self_ + kSbPnext);
        g_.w32(self_ + kSbIGend, self_ + kSbGend);
        g_.w32(self_ + kSbIPend, self_ + kSbPend);
        setp(0, 0);
        setg(0, 0, 0);
    }

    // Whether the object and the six words its indirect pointers lead to are guest memory.
    bool valid(std::string_view fn) const
    {
        if (!g_.need(fn, self_, kSbBytes)) return false;
        for (const u32 f : {kSbIGfirst, kSbIPfirst, kSbIGnext, kSbIPnext, kSbIGend, kSbIPend})
            if (!g_.need(fn, ind(f), 4)) return false;
        return true;
    }

private:
    const G& g_;
    u32 self_;
};

// basic_streambuf(): the vptr, the lock, a locale (the global one) and no buffers.
template <u32 E>
void streambuf_construct(PPUContext& ctx, u32 self)
{
    G g(ctx);
    g.w32(self, vtable(ctx, Names<E>::sb));
    mutex_ctor_(ctx, self + kSbLock);
    const u32 loc = op_new(ctx, 4);
    locale_default(ctx, loc);
    g.w32(self + kSbPloc, loc);
    Sb<E>(g, self).init();
}

// ~basic_streambuf: the locale deleted, the lock destroyed.
template <u32 E>
void streambuf_destroy(PPUContext& ctx, u32 self)
{
    G g(ctx);
    g.w32(self, vtable(ctx, Names<E>::sb));
    if (const u32 loc = g.r32(self + kSbPloc)) {
        locale_destroy(ctx, loc);
        op_delete(ctx, loc);
    }
    mutex_dtor_(ctx, self + kSbLock);
}

template <u32 E, bool Deleting>
void sb_dtor_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (!g.need("basic_streambuf::~basic_streambuf", self, kSbBytes)) return;
    streambuf_destroy<E>(ctx, self);
    if (Deleting) op_delete(ctx, self);
}

template <u32 E>
void sb_lock_(PPUContext& ctx, u32 self)
{
    mutex_lock_(ctx, self + kSbLock);
}
template <u32 E>
void sb_unlock_(PPUContext& ctx, u32 self)
{
    mutex_unlock_(ctx, self + kSbLock);
}
template <u32 E>
s32 sb_eof_(PPUContext&, u32, s32)
{
    return kEof; // overflow, pbackfail
}
template <u32 E>
s32 sb_underflow_(PPUContext&, u32)
{
    return kEof;
}
template <u32 E>
s32 sb_showmanyc_(PPUContext&, u32)
{
    return 0;
}
template <u32 E>
s32 sb_sync_(PPUContext&, u32)
{
    return 0;
}
template <u32 E>
u32 sb_setbuf_(PPUContext&, u32 self, u32, s32)
{
    return self;
}
template <u32 E>
void sb_imbue_(PPUContext&, u32, u32)
{
}
template <u32 E>
void sb_seekoff_(PPUContext& ctx, u32 out, u32, s32, s32, s32)
{
    G g(ctx);
    if (g.need("basic_streambuf::seekoff", out, 24)) pos_off(g, out, kBadoff);
}
template <u32 E>
void sb_seekpos_(PPUContext& ctx, u32 out, u32, u64, u64, u64, s32)
{
    G g(ctx);
    if (g.need("basic_streambuf::seekpos", out, 24)) pos_off(g, out, kBadoff);
}

// The virtual functions by slot: [~D1, ~D0, _Lock, _Unlock, overflow, pbackfail, showmanyc, underflow, uflow, xsgetn,
// xsputn, seekoff, seekpos, setbuf, sync, imbue].
enum SbSlot : u32 {
    kSlotLock = 2, kSlotUnlock = 3, kSlotOverflow = 4, kSlotPbackfail = 5, kSlotShowmanyc = 6, kSlotUnderflow = 7,
    kSlotUflow = 8, kSlotXsgetn = 9, kSlotXsputn = 10, kSlotSeekoff = 11, kSlotSeekpos = 12, kSlotSetbuf = 13,
    kSlotSync = 14, kSlotImbue = 15
};

inline s32 v_overflow(PPUContext& ctx, u32 sb, s32 meta)
{
    return static_cast<s32>(static_cast<u32>(vcall(ctx, sb, kSlotOverflow, {sb, static_cast<u64>(static_cast<s64>(meta))},
        static_cast<u64>(kEof))));
}
inline s32 v_underflow(PPUContext& ctx, u32 sb)
{
    return static_cast<s32>(static_cast<u32>(vcall(ctx, sb, kSlotUnderflow, {sb}, static_cast<u64>(kEof))));
}
inline s32 v_uflow(PPUContext& ctx, u32 sb)
{
    return static_cast<s32>(static_cast<u32>(vcall(ctx, sb, kSlotUflow, {sb}, static_cast<u64>(kEof))));
}
inline s32 v_pbackfail(PPUContext& ctx, u32 sb, s32 meta)
{
    return static_cast<s32>(static_cast<u32>(
        vcall(ctx, sb, kSlotPbackfail, {sb, static_cast<u64>(static_cast<s64>(meta))}, static_cast<u64>(kEof))));
}
inline s32 v_sync(PPUContext& ctx, u32 sb)
{
    return static_cast<s32>(static_cast<u32>(vcall(ctx, sb, kSlotSync, {sb}, static_cast<u64>(kEof))));
}

// uflow(): underflow, then the element it found, passed.
template <u32 E>
s32 sb_uflow_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    const Sb<E> sb(g, self);
    if (!sb.valid("basic_streambuf::uflow")) return kEof;
    if (v_underflow(ctx, self) == kEof) return kEof;
    const u32 p = sb.gptr();
    sb.gbump(1);
    return to_int<E>(sb.el(p));
}

// xsgetn(_Elem *, streamsize): from the read buffer, else element by element through uflow.
template <u32 E>
s32 sb_xsgetn_(PPUContext& ctx, u32 self, u32 ptr, s32 count)
{
    G g(ctx);
    const Sb<E> sb(g, self);
    if (!sb.valid("basic_streambuf::xsgetn")) return 0;
    s32 copied = 0;
    while (0 < count) {
        s32 size = sb.gnavail();
        if (0 < size) {
            if (count < size) size = count;
            if (!g.need("basic_streambuf::xsgetn", ptr, u64(size) * E) ||
                !g.need("basic_streambuf::xsgetn", sb.gptr(), u64(size) * E))
                break;
            std::memmove(g.b + ptr, g.b + sb.gptr(), u64(size) * E);
            ptr += static_cast<u32>(size) * E;
            copied += size;
            count -= size;
            sb.gbump(size);
        } else {
            const s32 meta = v_uflow(ctx, self);
            if (meta == kEof) break;
            if (!g.need("basic_streambuf::xsgetn", ptr, E)) break;
            g.template wel<E>(ptr, static_cast<u32>(meta));
            ptr += E;
            ++copied;
            --count;
        }
    }
    return copied;
}

// xsputn(const _Elem *, streamsize): into the write buffer, else element by element through overflow.
template <u32 E>
s32 sb_xsputn_(PPUContext& ctx, u32 self, u32 ptr, s32 count)
{
    G g(ctx);
    const Sb<E> sb(g, self);
    if (!sb.valid("basic_streambuf::xsputn")) return 0;
    s32 copied = 0;
    while (0 < count) {
        s32 size = sb.pnavail();
        if (0 < size) {
            if (count < size) size = count;
            if (!g.need("basic_streambuf::xsputn", ptr, u64(size) * E) ||
                !g.need("basic_streambuf::xsputn", sb.pptr(), u64(size) * E))
                break;
            std::memmove(g.b + sb.pptr(), g.b + ptr, u64(size) * E);
            ptr += static_cast<u32>(size) * E;
            copied += size;
            count -= size;
            sb.pbump(size);
        } else {
            if (!g.need("basic_streambuf::xsputn", ptr, E)) break;
            if (v_overflow(ctx, self, to_int<E>(sb.el(ptr))) == kEof) break;
            ptr += E;
            ++copied;
            --count;
        }
    }
    return copied;
}

// The inline members the library's own code uses: sgetc, sbumpc, sputc, sungetc (as the header has them).
template <u32 E>
s32 sgetc(PPUContext& ctx, u32 sb_addr)
{
    G g(ctx);
    const Sb<E> sb(g, sb_addr);
    if (!sb.valid("basic_streambuf::sgetc")) return kEof;
    return 0 < sb.gnavail() ? to_int<E>(sb.el(sb.gptr())) : v_underflow(ctx, sb_addr);
}

template <u32 E>
s32 sbumpc(PPUContext& ctx, u32 sb_addr)
{
    G g(ctx);
    const Sb<E> sb(g, sb_addr);
    if (!sb.valid("basic_streambuf::sbumpc")) return kEof;
    if (0 < sb.gnavail()) {
        const u32 p = sb.gptr();
        sb.gbump(1);
        return to_int<E>(sb.el(p));
    }
    return v_uflow(ctx, sb_addr);
}

template <u32 E>
s32 sputc(PPUContext& ctx, u32 sb_addr, u32 ch)
{
    G g(ctx);
    const Sb<E> sb(g, sb_addr);
    if (!sb.valid("basic_streambuf::sputc")) return kEof;
    if (0 < sb.pnavail()) {
        const u32 p = sb.pptr();
        sb.pbump(1);
        g.template wel<E>(p, ch);
        return to_int<E>(ch);
    }
    return v_overflow(ctx, sb_addr, to_int<E>(ch));
}

// ------------------------------------------------------------------------------------------------- ios_base

// The standard streams' table (ios_base::_Addstd): slots 1 to 8 (the eighth when the first seven are taken).
struct StdStreams {
    std::mutex mutex;
    std::array<u32, 10> stdstr{}; // [1, 8]: the SDK's _NSTDSTR + 2
    std::array<u8, 10> stdopens{};
    s32 init_cnt = -1;  // ios_base::Init::_Init_cnt
    s32 winit_cnt = -1; // _Winit's
};

inline StdStreams& std_streams() { return HleProcess::current()->state<StdStreams>(); }

// _Callfns(event): each callback, most recently registered first: (*_Pfn)(event, *this, index).
void ios_callfns_(PPUContext& ctx, u32 self, s32 ev)
{
    G g(ctx);
    if (!g.need("ios_base::_Callfns", self, kIosBaseBytes)) return;
    for (u32 p = g.r32(self + kIosCalls); p;) {
        if (!g.need("ios_base::_Callfns", p, 12)) return;
        const u32 next = g.r32(p), index = g.r32(p + 4), fn = g.r32(p + 8);
        call_desc(ctx, fn, {static_cast<u64>(static_cast<s64>(ev)), self, static_cast<u64>(static_cast<s64>(static_cast<s32>(index)))});
        p = next;
    }
}

// _Tidy(): erase_event, then the arrays and the callbacks freed.
void ios_tidy_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (!g.need("ios_base::_Tidy", self, kIosBaseBytes)) return;
    ios_callfns_(ctx, self, 0);
    for (u32 p = g.r32(self + kIosArr); p;) {
        const u32 next = g.ok(p, 4) ? g.r32(p) : 0;
        op_delete(ctx, p);
        p = next;
    }
    g.w32(self + kIosArr, 0);
    for (u32 p = g.r32(self + kIosCalls); p;) {
        const u32 next = g.ok(p, 4) ? g.r32(p) : 0;
        op_delete(ctx, p);
        p = next;
    }
    g.w32(self + kIosCalls, 0);
}

// clear(iostate, bool _Reraise): the state; failure raised for a state the exception mask asks for.
inline void ios_clear(PPUContext& ctx, u32 self, u32 state, bool reraise)
{
    G g(ctx);
    const u32 st = state & kStatmask;
    g.w32(self + kIosState, st);
    const u32 hit = st & g.r32(self + kIosExcept);
    if (hit == 0 || reraise) return; // _RERAISE has nothing to rethrow in the no-exception library
    if (hit & kBadbit) raise_new(ctx, kFailure, "ios_base::badbit set");
    if (hit & kFailbit) raise_new(ctx, kFailure, "ios_base::failbit set");
    raise_new(ctx, kFailure, "ios_base::eofbit set");
}

void ios_clear_(PPUContext& ctx, u32 self, u32 state, bool reraise)
{
    G g(ctx);
    if (g.need("ios_base::clear", self, kIosBaseBytes)) ios_clear(ctx, self, state, reraise);
}

inline void ios_setstate(PPUContext& ctx, u32 self, u32 bits)
{
    G g(ctx);
    ios_clear(ctx, self, g.r32(self + kIosState) | bits, false);
}

// _Init(): the default format (skipws | dec, precision 6, width 0), no state, a new locale (the global one).
inline void ios_init(PPUContext& ctx, u32 self)
{
    G g(ctx);
    g.w32(self + kIosPloc, 0);
    g.w32(self + kIosStdstr, 0);
    g.w32(self + kIosExcept, kGoodbit);
    g.w32(self + kIosFmtfl, kSkipws | kDec);
    g.w32(self + kIosPrec, 6);
    g.w32(self + kIosWide, 0);
    g.w32(self + kIosArr, 0);
    g.w32(self + kIosCalls, 0);
    ios_clear(ctx, self, kGoodbit, false);
    const u32 loc = op_new(ctx, 4);
    locale_default(ctx, loc);
    g.w32(self + kIosPloc, loc);
}

void ios_init_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (g.need("ios_base::_Init", self, kIosBaseBytes)) ios_init(ctx, self);
}

// _Addstd(): the stream takes a slot of the standard streams' table (its own, or a free one) and counts an open.
void ios_addstd_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (!g.need("ios_base::_Addstd", self, kIosBaseBytes)) return;
    Lockit lock(ctx, kLockStream);
    StdStreams& s = std_streams();
    std::lock_guard l(s.mutex);
    u32 n = 0;
    while (++n < 8)
        if (s.stdstr[n] == 0 || s.stdstr[n] == self) break;
    g.w32(self + kIosStdstr, n);
    s.stdstr[n] = self;
    ++s.stdopens[n];
}

// register_callback(event_callback, int): a node in front of the list.
void ios_register_callback_(PPUContext& ctx, u32 self, u32 fn, s32 index)
{
    G g(ctx);
    if (!g.need("ios_base::register_callback", self, kIosBaseBytes)) return;
    const u32 node = op_new(ctx, 12);
    g.w32(node, g.r32(self + kIosCalls));
    g.w32(node + 4, static_cast<u32>(index));
    g.w32(node + 8, fn);
    g.w32(self + kIosCalls, node);
}

// _Findarr(int): the element for the index (or a recycled empty one, or a new one in front).
inline u32 ios_findarr(PPUContext& ctx, u32 self, s32 index)
{
    G g(ctx);
    u32 q = 0;
    for (u32 p = g.r32(self + kIosArr); p; p = g.r32(p)) {
        if (!g.need("ios_base::_Findarr", p, 16)) return 0;
        if (static_cast<s32>(g.r32(p + 4)) == index) return p;
        if (q == 0 && g.r32(p + 8) == 0 && g.r32(p + 12) == 0) q = p;
    }
    if (q) {
        g.w32(q + 4, static_cast<u32>(index));
        return q;
    }
    const u32 node = op_new(ctx, 16);
    g.w32(node, g.r32(self + kIosArr));
    g.w32(node + 4, static_cast<u32>(index));
    g.w32(node + 8, 0);
    g.w32(node + 12, 0);
    g.w32(self + kIosArr, node);
    return node;
}

u32 ios_findarr_(PPUContext& ctx, u32 self, s32 index)
{
    G g(ctx);
    return g.need("ios_base::_Findarr", self, kIosBaseBytes) ? ios_findarr(ctx, self, index) : 0;
}

// copyfmt(const ios_base&): everything but the state; the arrays' nonzero values and the callbacks copied, then
// copyfmt_event, then the other's exception mask (which may raise).
u32 ios_copyfmt_(PPUContext& ctx, u32 self, u32 other)
{
    G g(ctx);
    if (!g.need("ios_base::copyfmt", self, kIosBaseBytes) || !g.need("ios_base::copyfmt", other, kIosBaseBytes))
        return self;
    if (self == other) return self;
    ios_tidy_(ctx, self);
    locale_assign(ctx, g.r32(self + kIosPloc), g.r32(other + kIosPloc));
    g.w32(self + kIosFmtfl, g.r32(other + kIosFmtfl));
    g.w32(self + kIosPrec, g.r32(other + kIosPrec));
    g.w32(self + kIosWide, g.r32(other + kIosWide));
    for (u32 p = g.r32(other + kIosArr); p && g.ok(p, 16); p = g.r32(p)) {
        if (g.r32(p + 8) != 0 || g.r32(p + 12) != 0) {
            const u32 a = ios_findarr(ctx, self, static_cast<s32>(g.r32(p + 4)));
            g.w32(a + 8, g.r32(p + 8));
            const u32 b = ios_findarr(ctx, self, static_cast<s32>(g.r32(p + 4)));
            g.w32(b + 12, g.r32(p + 12));
        }
    }
    // The callbacks, in the other's order (each one put in front, as register_callback does).
    for (u32 q = g.r32(other + kIosCalls); q && g.ok(q, 12); q = g.r32(q))
        ios_register_callback_(ctx, self, g.r32(q + 8), static_cast<s32>(g.r32(q + 4)));
    ios_callfns_(ctx, self, 2);
    // exceptions(_Other._Except): the mask, then clear(rdstate()).
    g.w32(self + kIosExcept, g.r32(other + kIosExcept) & kStatmask);
    ios_clear(ctx, self, g.r32(self + kIosState), false);
    return self;
}

// imbue(const locale&): the old locale (in *out), the new one taken, imbue_event.
void ios_imbue_(PPUContext& ctx, u32 out, u32 self, u32 loc)
{
    G g(ctx);
    if (!g.need("ios_base::imbue", out, 4) || !g.need("ios_base::imbue", self, kIosBaseBytes) ||
        !g.need("ios_base::imbue", loc, 4))
        return;
    const u32 ploc = g.r32(self + kIosPloc);
    locale_copy(ctx, out, ploc);
    locale_assign(ctx, ploc, loc);
    ios_callfns_(ctx, self, 1);
}

// ~ios_base: a standard stream's last close only; _Tidy and the locale deleted.
inline void ios_base_destroy(PPUContext& ctx, u32 self)
{
    G g(ctx);
    g.w32(self, vtable(ctx, "St8ios_base"));
    if (const u32 n = g.r32(self + kIosStdstr); n != 0 && n < 10) {
        StdStreams& s = std_streams();
        std::lock_guard l(s.mutex);
        if (0 < static_cast<s8>(--s.stdopens[n])) return;
    }
    ios_tidy_(ctx, self);
    if (const u32 loc = g.r32(self + kIosPloc)) {
        locale_destroy(ctx, loc);
        op_delete(ctx, loc);
    }
}

template <bool Deleting>
void ios_base_dtor_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (!g.need("ios_base::~ios_base", self, kIosBaseBytes)) return;
    ios_base_destroy(ctx, self);
    if (Deleting) op_delete(ctx, self);
}

// ios_base::failure's destructors: those of runtime_error with failure's vtable first.
template <bool Deleting>
void failure_dtor_(PPUContext& ctx, u32 self)
{
    exception_dtor(ctx, self, kFailure, Deleting);
}

// ------------------------------------------------------------------------------------------------- basic_ios

// basic_ios() (the protected default constructor the streams' complete-object constructors run): the vptr only.
template <u32 E>
void basic_ios_construct(PPUContext& ctx, u32 self)
{
    G g(ctx);
    g.w32(self, vtable(ctx, Names<E>::ios));
}

// widen(char): use_facet<ctype<_Elem> >(getloc()).widen(c), ctype's do_widen(char) (slot 6 for char, 10 for wchar_t).
template <u32 E>
u32 ios_widen(PPUContext& ctx, u32 ios, u32 ch)
{
    G g(ctx);
    const u32 ct = use_facet(ctx, g.r32(ios + kIosPloc), Names<E>::ctype);
    const u32 r = static_cast<u32>(vcall(ctx, ct, E == 1 ? 6 : 10, {ct, ch & 0xFF}, ch));
    return E == 1 ? (r & 0xFF) : r;
}

// init(basic_streambuf *, bool _Isstd)
template <u32 E>
void basic_ios_init(PPUContext& ctx, u32 self, u32 sb, bool isstd)
{
    G g(ctx);
    ios_init(ctx, self);
    g.w32(self + kIosSb, sb);
    g.w32(self + kIosTie, 0);
    g.template wel<E>(self + kIosFill, ios_widen<E>(ctx, self, ' '));
    if (sb == 0) ios_setstate(ctx, self, kBadbit);
    if (isstd) ios_addstd_(ctx, self);
    else g.w32(self + kIosStdstr, 0);
}

template <u32 E>
void basic_ios_init_(PPUContext& ctx, u32 self, u32 sb, bool isstd)
{
    G g(ctx);
    if (g.need("basic_ios::init", self, kBasicIosBytes)) basic_ios_init<E>(ctx, self, sb, isstd);
}

// ~basic_ios: nothing of its own; ios_base's.
template <u32 E>
void basic_ios_destroy(PPUContext& ctx, u32 self)
{
    G g(ctx);
    g.w32(self, vtable(ctx, Names<E>::ios));
    ios_base_destroy(ctx, self);
}

template <u32 E, bool Deleting>
void basic_ios_dtor_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (!g.need("basic_ios::~basic_ios", self, kBasicIosBytes)) return;
    basic_ios_destroy<E>(ctx, self);
    if (Deleting) op_delete(ctx, self);
}

// ------------------------------------------------------------------------------------------------- basic_filebuf

namespace libc_fn {
inline const Callee fgetwc{"sys_libc", "fgetwc"}, fputwc{"sys_libc", "fputwc"}, ungetwc{"sys_libc", "ungetwc"};
} // namespace libc_fn

// The C stream's element functions: fgetc/fputc/ungetc for char, their wide forms for wchar_t.
template <u32 E>
struct ElemIo {
    static const Callee& getc() { return E == 1 ? libc_fn::fgetc : libc_fn::fgetwc; }
    static const Callee& putc() { return E == 1 ? libc_fn::fputc : libc_fn::fputwc; }
    static const Callee& ungetc() { return E == 1 ? libc_fn::ungetc : libc_fn::ungetwc; }
};

inline constexpr u32 kWeof = 0xFFFFFFFFu;

// _Fputc, _Fgetc, _Ungetc: the C stream's element functions (fputc/fputwc...).
template <u32 E>
bool fputc_el(PPUContext& ctx, u32 ch, u32 file)
{
    const u32 r = static_cast<u32>(ElemIo<E>::putc()(ctx, {E == 1 ? (ch & 0xFF) : ch, file}));
    return E == 1 ? static_cast<s32>(r) != kEof : r != kWeof;
}

template <u32 E>
std::optional<u32> fgetc_el(PPUContext& ctx, u32 file)
{
    const u32 r = static_cast<u32>(ElemIo<E>::getc()(ctx, {file}));
    if (E == 1 ? static_cast<s32>(r) == kEof : r == kWeof) return std::nullopt;
    return r;
}

template <u32 E>
bool ungetc_el(PPUContext& ctx, u32 ch, u32 file)
{
    const u32 r = static_cast<u32>(ElemIo<E>::ungetc()(ctx, {E == 1 ? (ch & 0xFF) : ch, file}));
    return E == 1 ? static_cast<s32>(r) != kEof : r != kWeof;
}

// codecvt's virtual functions (codecvt_base: [~D1, ~D0, do_always_noconv, do_max_length, do_encoding], then do_in,
// do_out, do_unshift, do_length) and its results.
enum CvtResult : s32 { kCvtOk = 0, kCvtPartial = 1, kCvtError = 2, kCvtNoconv = 3 };

template <u32 E>
class Filebuf {
public:
    using L = FbLayout<E>;
    Filebuf(PPUContext& ctx, u32 self) : ctx_(ctx), g_(ctx), self_(self) {}

    u32 file() const { return g_.r32(self_ + L::kFile); }
    u32 pcvt() const { return g_.r32(self_ + L::kPcvt); }
    u32 mychar() const { return self_ + L::kMychar; }
    Sb<E> sb() const { return Sb<E>(g_, self_); }

    bool valid(std::string_view fn) const { return g_.need(fn, self_, L::kBytes) && sb().valid(fn); }

    // _Init(FILE *, _Initfl): the stream buffer's own pointers (sys_libc's FILE keeps no buffer for it to share),
    // the file, the initial state, no codecvt.
    void init(u32 f, s32 which) const
    {
        g_.w8(self_ + L::kClosef, which == 1 ? 1 : 0);
        g_.w8(self_ + L::kWrotesome, 0);
        sb().init();
        g_.w32(self_ + L::kFile, f);
        g_.w64(self_ + L::kState, 0);
        g_.w32(self_ + L::kPcvt, 0);
    }

    // _Initcvt(codecvt *)
    void initcvt(u32 cvt) const
    {
        if (static_cast<u8>(vcall(ctx_, cvt, 2, {cvt}, 1)) != 0) {
            g_.w32(self_ + L::kPcvt, 0);
        } else {
            g_.w32(self_ + L::kPcvt, cvt);
            sb().init();
        }
    }

    // _Endwrite(): the conversion state homed (unshift) and written.
    bool endwrite() const
    {
        if (pcvt() == 0 || !g_.r8(self_ + L::kWrotesome)) return true;
        if (v_overflow(ctx_, self_, kEof) == kEof) return false;
        u32 size = 8;
        StackVar buf(ctx_, g_.b, 40);
        const u32 str = buf.addr(), dest = buf.addr() + 32;
        for (;;) {
            const s32 r = static_cast<s32>(static_cast<u32>(vcall(ctx_, pcvt(), 7,
                {pcvt(), self_ + L::kState, str, str + size, dest}, static_cast<u64>(kCvtError))));
            switch (r) {
            case kCvtOk: g_.w8(self_ + L::kWrotesome, 0); [[fallthrough]];
            case kCvtPartial: {
                const u32 count = g_.r32(dest) - str;
                if (0 < count && count != static_cast<u32>(libc_fn::fwrite(ctx_, {str, 1, count, file()}))) return false;
                if (!g_.r8(self_ + L::kWrotesome)) return true;
                if (count == 0) {
                    if (size >= 32) return false;
                    size += 8;
                }
                break;
            }
            case kCvtNoconv: return true;
            default: return false;
            }
        }
    }

    // close(): the homing sequence, fclose, then _Init(0, _Closefl); null when anything failed.
    u32 close() const
    {
        u32 ans = self_;
        if (file() == 0) {
            ans = 0;
        } else {
            if (!endwrite()) ans = 0;
            if (static_cast<s32>(static_cast<u32>(libc_fn::fclose(ctx_, {file()}))) != 0) ans = 0;
        }
        init(0, 2);
        return ans;
    }

private:
    PPUContext& ctx_;
    G g_;
    u32 self_;
};

// basic_filebuf(FILE *): the stream buffer, then _Init(_File, _Newfl).
template <u32 E>
void filebuf_construct(PPUContext& ctx, u32 self, u32 file)
{
    streambuf_construct<E>(ctx, self);
    G g(ctx);
    g.w32(self, vtable(ctx, Names<E>::fb));
    Filebuf<E>(ctx, self).init(file, 0);
}

template <u32 E>
void filebuf_init_(PPUContext& ctx, u32 self, u32 file, s32 which)
{
    G g(ctx);
    if (g.need("basic_filebuf::_Init", self, FbLayout<E>::kBytes)) Filebuf<E>(ctx, self).init(file, which);
}

template <u32 E, bool Deleting>
void filebuf_dtor_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    const Filebuf<E> fb(ctx, self);
    if (!fb.valid("basic_filebuf::~basic_filebuf")) return;
    g.w32(self, vtable(ctx, Names<E>::fb));
    if (g.r8(self + FbLayout<E>::kClosef)) fb.close();
    streambuf_destroy<E>(ctx, self);
    if (Deleting) op_delete(ctx, self);
}

template <u32 E>
void filebuf_lock_(PPUContext& ctx, u32 self)
{
    const Filebuf<E> fb(ctx, self);
    if (fb.valid("basic_filebuf::_Lock") && fb.file()) libc_fn::lockfile(ctx, {fb.file()});
}

template <u32 E>
void filebuf_unlock_(PPUContext& ctx, u32 self)
{
    const Filebuf<E> fb(ctx, self);
    if (fb.valid("basic_filebuf::_Unlock") && fb.file()) libc_fn::unlockfile(ctx, {fb.file()});
}

// imbue(const locale&): the locale's codecvt taken when it converts.
template <u32 E>
void filebuf_imbue_(PPUContext& ctx, u32 self, u32 loc)
{
    const Filebuf<E> fb(ctx, self);
    if (!fb.valid("basic_filebuf::imbue")) return;
    fb.initcvt(use_facet(ctx, loc, Names<E>::codecvt));
}

// overflow(int_type): into the buffer when there is room, else to the file, converted when there is a codecvt.
template <u32 E>
s32 filebuf_overflow_(PPUContext& ctx, u32 self, s32 meta)
{
    G g(ctx);
    const Filebuf<E> fb(ctx, self);
    if (!fb.valid("basic_filebuf::overflow")) return kEof;
    const Sb<E> sb = fb.sb();
    if (meta == kEof) return 0; // not_eof(eof)
    if (sb.pptr() != 0 && sb.pptr() < sb.epptr()) {
        const u32 p = sb.pptr();
        sb.pbump(1);
        g.template wel<E>(p, static_cast<u32>(meta));
        return meta;
    }
    const u32 file = fb.file();
    if (file == 0) return kEof;
    if (fb.pcvt() == 0) return fputc_el<E>(ctx, static_cast<u32>(meta), file) ? meta : kEof;
    // _Ch, _Src, _Dest and the string the conversion writes to, on the guest stack.
    StackVar buf(ctx, g.b, 64);
    const u32 ch = buf.addr(), src = buf.addr() + 8, dest = buf.addr() + 12, str = buf.addr() + 16;
    g.template wel<E>(ch, static_cast<u32>(meta));
    u32 size = 8;
    for (;;) {
        const u32 cvt = fb.pcvt();
        const s32 r = static_cast<s32>(static_cast<u32>(vcall(ctx, cvt, 6,
            {cvt, self + FbLayout<E>::kState, ch, ch + E, src, str, str + size, dest}, static_cast<u64>(kCvtError))));
        switch (r) {
        case kCvtPartial:
        case kCvtOk: {
            const u32 count = g.r32(dest) - str;
            if (0 < count && count != static_cast<u32>(libc_fn::fwrite(ctx, {str, 1, count, file}))) return kEof;
            g.w8(self + FbLayout<E>::kWrotesome, 1);
            if (g.r32(src) != ch) return meta;
            if (0 < count) {
            } else if (size < 32) {
                size += 8;
            } else {
                return kEof;
            }
            break;
        }
        case kCvtNoconv: return fputc_el<E>(ctx, static_cast<u32>(meta), file) ? meta : kEof;
        default: return kEof;
        }
    }
}

// pbackfail(int_type): back up in the buffer; else ungetc to the file (no codecvt); else into _Mychar.
template <u32 E>
s32 filebuf_pbackfail_(PPUContext& ctx, u32 self, s32 meta)
{
    G g(ctx);
    const Filebuf<E> fb(ctx, self);
    if (!fb.valid("basic_filebuf::pbackfail")) return kEof;
    const Sb<E> sb = fb.sb();
    if (sb.gptr() != 0 && sb.eback() < sb.gptr() &&
        (meta == kEof || to_int<E>(sb.el(sb.gptr() - E)) == meta)) {
        sb.gbump(-1);
        return meta == kEof ? 0 : meta;
    }
    if (fb.file() == 0 || meta == kEof) return kEof;
    if (fb.pcvt() == 0 && ungetc_el<E>(ctx, static_cast<u32>(meta), fb.file())) return meta;
    if (sb.gptr() != fb.mychar()) {
        g.template wel<E>(fb.mychar(), static_cast<u32>(meta));
        sb.setg(fb.mychar(), fb.mychar(), fb.mychar() + E);
        return meta;
    }
    return kEof;
}

// uflow(): the buffered element, else one from the file (through the codecvt, byte by byte, when there is one).
template <u32 E>
s32 filebuf_uflow_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    const Filebuf<E> fb(ctx, self);
    if (!fb.valid("basic_filebuf::uflow")) return kEof;
    const Sb<E> sb = fb.sb();
    if (sb.gptr() != 0 && sb.gptr() < sb.egptr()) {
        const u32 p = sb.gptr();
        sb.gbump(1);
        return to_int<E>(sb.el(p));
    }
    const u32 file = fb.file();
    if (file == 0) return kEof;
    if (fb.pcvt() == 0) {
        const auto c = fgetc_el<E>(ctx, file);
        return c ? to_int<E>(*c) : kEof;
    }
    // _Ch, _Dest, _Src and the bytes read so far (at most 64), on the guest stack.
    StackVar buf(ctx, g.b, 96);
    const u32 ch = buf.addr(), dest = buf.addr() + 8, src = buf.addr() + 12, str = buf.addr() + 32;
    u32 len = 0;
    for (;;) {
        const s32 b = static_cast<s32>(static_cast<u32>(libc_fn::fgetc(ctx, {file})));
        if (b == kEof) return kEof;
        if (len == 64) return kEof; // no element in 64 bytes: the SDK's string would grow; no codecvt needs that
        g.w8(str + len++, static_cast<u8>(b));
        const u32 cvt = fb.pcvt();
        const s32 r = static_cast<s32>(static_cast<u32>(vcall(ctx, cvt, 5,
            {cvt, self + FbLayout<E>::kState, str, str + len, src, ch, ch + E, dest}, static_cast<u64>(kCvtError))));
        switch (r) {
        case kCvtPartial:
        case kCvtOk:
            if (g.r32(dest) != ch) {
                // An element: the excess bytes back to the file.
                for (u32 left = str + len - g.r32(src); 0 < left;) ungetc_el<1>(ctx, g.r8(g.r32(src) + --left), file);
                return to_int<E>(g.template rel<E>(ch));
            } else {
                // Partial: the bytes used dropped.
                const u32 used = g.r32(src) - str;
                std::memmove(g.b + str, g.b + str + used, len - used);
                len -= used;
            }
            break;
        case kCvtNoconv:
            if (len < E) break;
            std::memcpy(g.b + ch, g.b + str, E);
            return to_int<E>(g.template rel<E>(ch));
        default: return kEof;
        }
    }
}

// underflow(): the buffered element, else uflow and put it back.
template <u32 E>
s32 filebuf_underflow_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    const Filebuf<E> fb(ctx, self);
    if (!fb.valid("basic_filebuf::underflow")) return kEof;
    const Sb<E> sb = fb.sb();
    if (sb.gptr() != 0 && sb.gptr() < sb.egptr()) return to_int<E>(sb.el(sb.gptr()));
    const s32 meta = v_uflow(ctx, self);
    if (meta == kEof) return meta;
    v_pbackfail(ctx, self, meta);
    return meta;
}

// seekoff(off_type, seekdir, openmode): fseek (a putback in _Mychar backed over), the position from ftell.
template <u32 E>
void filebuf_seekoff_(PPUContext& ctx, u32 out, u32 self, s32 off, s32 way, s32)
{
    G g(ctx);
    if (!g.need("basic_filebuf::seekoff", out, 24)) return;
    const Filebuf<E> fb(ctx, self);
    if (!fb.valid("basic_filebuf::seekoff")) return pos_off(g, out, kBadoff);
    const Sb<E> sb = fb.sb();
    if (sb.gptr() == fb.mychar() && way == 1 && fb.pcvt() == 0) off -= static_cast<s32>(E);
    const u32 file = fb.file();
    s32 pos = kBadoff;
    if (file == 0 || !fb.endwrite() ||
        ((off != 0 || way != 1) &&
            static_cast<s32>(static_cast<u32>(libc_fn::fseek(ctx, {file, static_cast<u64>(static_cast<s64>(off)),
                static_cast<u64>(static_cast<s64>(way))}))) != 0) ||
        (pos = static_cast<s32>(static_cast<u32>(libc_fn::ftell(ctx, {file})))) == kBadoff)
        return pos_off(g, out, kBadoff);
    if (sb.gptr() == fb.mychar()) sb.setg(fb.mychar(), fb.mychar() + E, fb.mychar() + E);
    pos_write(g, out, 0, pos, 0, g.r64(self + FbLayout<E>::kState));
}

// seekpos(pos_type, openmode): to the position's file offset, then by its stream offset; its state taken.
template <u32 E>
void filebuf_seekpos_(PPUContext& ctx, u32 out, u32 self, u64 p0, u64 p1, u64 p2, s32)
{
    G g(ctx);
    if (!g.need("basic_filebuf::seekpos", out, 24)) return;
    const Filebuf<E> fb(ctx, self);
    if (!fb.valid("basic_filebuf::seekpos")) return pos_off(g, out, kBadoff);
    const Pos pos(p0, p1, p2);
    const s32 off = pos.myoff;
    const u32 file = fb.file();
    s32 now = kBadoff;
    if (file == 0 || !fb.endwrite() ||
        static_cast<s32>(static_cast<u32>(libc_fn::fseek(ctx, {file, static_cast<u64>(static_cast<s64>(pos.fpos_off)), 0}))) != 0 ||
        (off != 0 &&
            static_cast<s32>(static_cast<u32>(libc_fn::fseek(ctx, {file, static_cast<u64>(static_cast<s64>(off)), 1}))) != 0) ||
        (now = static_cast<s32>(static_cast<u32>(libc_fn::ftell(ctx, {file})))) == kBadoff)
        return pos_off(g, out, kBadoff);
    g.w64(self + FbLayout<E>::kState, pos.mystate);
    const Sb<E> sb = fb.sb();
    if (sb.gptr() == fb.mychar()) sb.setg(fb.mychar(), fb.mychar() + E, fb.mychar() + E);
    pos_write(g, out, 0, now, 0, pos.mystate);
}

// setbuf(_Elem *, streamsize): the buffer offered to the C stream (unbuffered for none), then _Init(file, _Openfl).
template <u32 E>
u32 filebuf_setbuf_(PPUContext& ctx, u32 self, u32 buffer, s32 count)
{
    const Filebuf<E> fb(ctx, self);
    if (!fb.valid("basic_filebuf::setbuf")) return 0;
    const u32 file = fb.file();
    if (file == 0) return 0;
    const s32 mode = buffer == 0 && count == 0 ? 2 : 0; // _IONBF, _IOFBF
    if (static_cast<s32>(static_cast<u32>(libc_fn::setvbuf(ctx, {file, buffer, static_cast<u64>(mode),
            static_cast<u64>(static_cast<u32>(count) * E)}))) != 0)
        return 0;
    fb.init(file, 1);
    return self;
}

// sync(): what is buffered written (overflow), then fflush.
template <u32 E>
s32 filebuf_sync_(PPUContext& ctx, u32 self)
{
    const Filebuf<E> fb(ctx, self);
    if (!fb.valid("basic_filebuf::sync")) return -1;
    if (fb.file() == 0) return 0;
    if (v_overflow(ctx, self, kEof) == kEof) return 0;
    return 0 <= static_cast<s32>(static_cast<u32>(libc_fn::fflush(ctx, {fb.file()}))) ? 0 : -1;
}

template <u32 E>
u32 filebuf_endwrite_(PPUContext& ctx, u32 self)
{
    const Filebuf<E> fb(ctx, self);
    return fb.valid("basic_filebuf::_Endwrite") && fb.endwrite() ? 1 : 0;
}

// _Fiopen(const char *, openmode, int): the mode checked against the valid combinations and opened with fopen;
// _Noreplace fails on an existing file, ate seeks to the end.
inline u32 fiopen(PPUContext& ctx, u32 path, u32 mode)
{
    static constexpr std::pair<u32, std::string_view> kValid[] = {{kIn, "r"}, {kOut, "w"}, {kOut | kTrunc, "w"},
        {kOut | kApp, "a"}, {kIn | kBinary, "rb"}, {kOut | kBinary, "wb"}, {kOut | kTrunc | kBinary, "wb"},
        {kOut | kApp | kBinary, "ab"}, {kIn | kOut, "r+"}, {kIn | kOut | kTrunc, "w+"}, {kIn | kOut | kApp, "a+"},
        {kIn | kOut | kBinary, "r+b"}, {kIn | kOut | kTrunc | kBinary, "w+b"}, {kIn | kOut | kApp | kBinary, "a+b"}};
    const u32 atend = mode & kAte, norep = mode & kNoreplace;
    if (mode & kNocreate) mode |= kIn;
    if (mode & kApp) mode |= kOut;
    mode &= ~(kAte | kNocreate | kNoreplace);
    const auto it = std::find_if(std::begin(kValid), std::end(kValid), [&](const auto& v) { return v.first == mode; });
    if (it == std::end(kValid)) return 0;
    if (norep && (mode & (kOut | kApp))) {
        if (const u32 fp = static_cast<u32>(libc_fn::fopen(ctx, {path, cstring(ctx, "r")}))) {
            libc_fn::fclose(ctx, {fp});
            return 0;
        }
    }
    const u32 fp = static_cast<u32>(libc_fn::fopen(ctx, {path, cstring(ctx, it->second)}));
    if (fp == 0) return 0;
    if (!atend || static_cast<s32>(static_cast<u32>(libc_fn::fseek(ctx, {fp, 0, 2}))) == 0) return fp;
    libc_fn::fclose(ctx, {fp});
    return 0;
}

u32 fiopen_(PPUContext& ctx, u32 path, u32 mode, s32)
{
    G g(ctx);
    if (!g.str(path)) {
        log_call_site(ctx, g.b, std::format("_Fiopen: {:#x} is not a file name", path));
        return 0;
    }
    return fiopen(ctx, path, mode);
}

// _Fiopen(const wchar_t *, ...): the wide name's characters narrowed (the "C" locale's bytes), then the same.
u32 wfiopen_(PPUContext& ctx, u32 path, u32 mode, s32)
{
    G g(ctx);
    const auto w = libc::read_wstring(g.t, path);
    if (!w) {
        log_call_site(ctx, g.b, std::format("_Fiopen: {:#x} is not a wide file name", path));
        return 0;
    }
    std::string name;
    for (const char32_t c : *w) {
        if (c > 0xFF) return 0; // no byte for it in the "C" locale
        name += static_cast<char>(c);
    }
    StackVar n(ctx, g.b, static_cast<u32>(name.size()) + 1);
    std::memcpy(g.b + n.addr(), name.data(), name.size());
    return fiopen(ctx, n.addr(), mode);
}

// ------------------------------------------------------------------------------------------------- strstreambuf

class Ssb {
public:
    Ssb(PPUContext& ctx, u32 self) : ctx_(ctx), g_(ctx), self_(self) {}
    Sb<1> sb() const { return Sb<1>(g_, self_); }
    u32 r(u32 f) const { return g_.r32(self_ + f); }
    void w(u32 f, u32 v) const { g_.w32(self_ + f, v); }
    bool valid(std::string_view fn) const { return g_.need(fn, self_, kSsBytes) && sb().valid(fn); }

    // _Init(streamsize, char *_Gp, char *_Pp, _Strstate)
    void init(s32 count, u32 gp, u32 pp, u32 mode) const
    {
        sb().init();
        w(kSsPendsave, 0);
        w(kSsSeekhigh, 0);
        w(kSsPalloc, 0);
        w(kSsPfree, 0);
        w(kSsStrmode, mode);
        if (gp == 0) {
            w(kSsAlsize, static_cast<u32>(kSsMinsize <= count ? count : kSsAlsizeDefault));
            w(kSsStrmode, r(kSsStrmode) | kSsDynamic);
            return;
        }
        s32 size = count;
        if (count < 0) size = 0x7FFFFFFF;
        else if (count == 0) size = static_cast<s32>(libc::guest_strnlen(g_.t, gp, libc::kNoLimit).value_or(0));
        w(kSsAlsize, 0);
        w(kSsSeekhigh, gp + static_cast<u32>(size));
        if (pp == 0) {
            sb().setg(gp, gp, gp + static_cast<u32>(size));
        } else {
            if (pp < gp) pp = gp;
            else if (gp + static_cast<u32>(size) < pp) pp = gp + static_cast<u32>(size);
            sb().setp(pp, gp + static_cast<u32>(size));
            sb().setg(gp, gp, pp);
        }
    }

    // _Tidy(): an allocated, unfrozen buffer freed (_Pfree or delete[]).
    void tidy() const
    {
        if ((r(kSsStrmode) & (kSsAllocated | kSsFrozen)) == kSsAllocated) {
            if (const u32 f = r(kSsPfree)) call_desc(ctx_, f, {sb().eback()});
            else op_delete(ctx_, sb().eback());
        }
        w(kSsSeekhigh, 0);
        w(kSsStrmode, r(kSsStrmode) & ~(kSsAllocated | kSsFrozen));
    }

private:
    PPUContext& ctx_;
    G g_;
    u32 self_;
};

// strstreambuf(char *_Getptr, streamsize _Count, char *_Putptr): the stream buffer, then _Init.
inline void strstreambuf_construct(PPUContext& ctx, u32 self, u32 gp, s32 count, u32 pp)
{
    streambuf_construct<1>(ctx, self);
    G g(ctx);
    g.w32(self, vtable(ctx, "St12strstreambuf"));
    Ssb(ctx, self).init(count, gp, pp, 0);
}

inline void strstreambuf_destroy(PPUContext& ctx, u32 self)
{
    G g(ctx);
    g.w32(self, vtable(ctx, "St12strstreambuf"));
    Ssb(ctx, self).tidy();
    streambuf_destroy<1>(ctx, self);
}

template <bool Deleting>
void ssb_dtor_(PPUContext& ctx, u32 self)
{
    const Ssb s(ctx, self);
    if (!s.valid("strstreambuf::~strstreambuf")) return;
    strstreambuf_destroy(ctx, self);
    if (Deleting) op_delete(ctx, self);
}

void ssb_init_(PPUContext& ctx, u32 self, s32 count, u32 gp, u32 pp, s32 mode)
{
    G g(ctx);
    if (g.need("strstreambuf::_Init", self, kSsBytes)) Ssb(ctx, self).init(count, gp, pp, static_cast<u32>(mode));
}

void ssb_tidy_(PPUContext& ctx, u32 self)
{
    const Ssb s(ctx, self);
    if (s.valid("strstreambuf::_Tidy")) s.tidy();
}

// freeze(bool): writing stopped (the end saved, the write area shrunk to the read area's start) or restarted.
void ssb_freeze_(PPUContext& ctx, u32 self, bool freezeit)
{
    const Ssb s(ctx, self);
    if (!s.valid("strstreambuf::freeze")) return;
    const Sb<1> sb = s.sb();
    const u32 mode = s.r(kSsStrmode);
    if (freezeit && !(mode & kSsFrozen)) {
        s.w(kSsStrmode, mode | kSsFrozen);
        s.w(kSsPendsave, sb.epptr());
        sb.setp(sb.pbase(), sb.pptr(), sb.eback());
    } else if (!freezeit && (mode & kSsFrozen)) {
        s.w(kSsStrmode, mode & ~kSsFrozen);
        sb.setp(sb.pbase(), sb.pptr(), s.r(kSsPendsave));
    }
}

// overflow(int): into the write area, else a larger array (_Palloc or new[]) when dynamic and not frozen.
s32 ssb_overflow_(PPUContext& ctx, u32 self, s32 meta)
{
    G g(ctx);
    const Ssb s(ctx, self);
    if (!s.valid("strstreambuf::overflow")) return kEof;
    const Sb<1> sb = s.sb();
    if (meta == kEof) return 0;
    const auto put = [&]() {
        const u32 p = sb.pptr();
        sb.pbump(1);
        g.w8(p, static_cast<u8>(meta));
        return meta & 0xFF;
    };
    if (sb.pptr() != 0 && sb.pptr() < sb.epptr()) return put();
    const u32 mode = s.r(kSsStrmode);
    if (!(mode & kSsDynamic) || (mode & (kSsConstant | kSsFrozen))) return kEof;
    const s32 osize = sb.gptr() == 0 ? 0 : static_cast<s32>(sb.epptr() - sb.eback());
    const s32 nsize = osize + static_cast<s32>(s.r(kSsAlsize));
    const u32 ptr = s.r(kSsPalloc) ? static_cast<u32>(call_desc(ctx, s.r(kSsPalloc), {static_cast<u32>(nsize)}))
                                   : op_new(ctx, static_cast<u32>(nsize));
    if (ptr == 0) return kEof;
    if (0 < osize) std::memcpy(g.b + ptr, g.b + sb.eback(), static_cast<u32>(osize));
    else if (kSsAlsizeDefault < static_cast<s32>(s.r(kSsAlsize))) s.w(kSsAlsize, kSsAlsizeDefault);
    if (mode & kSsAllocated) {
        if (const u32 f = s.r(kSsPfree)) call_desc(ctx, f, {sb.eback()});
        else op_delete(ctx, sb.eback());
    }
    s.w(kSsStrmode, s.r(kSsStrmode) | kSsAllocated);
    if (osize == 0) {
        s.w(kSsSeekhigh, ptr);
        sb.setp(ptr, ptr + static_cast<u32>(nsize));
        sb.setg(ptr, ptr, ptr);
    } else {
        const u32 old = sb.eback();
        s.w(kSsSeekhigh, s.r(kSsSeekhigh) - old + ptr);
        const u32 pnext = sb.pptr() - old + ptr;
        sb.setp(sb.pbase() - old + ptr, pnext, ptr + static_cast<u32>(nsize));
        sb.setg(ptr, sb.gptr() - old + ptr, pnext + 1);
    }
    return put();
}

// pbackfail(int): back up one, writing the character unless the array is constant and it differs.
s32 ssb_pbackfail_(PPUContext& ctx, u32 self, s32 meta)
{
    G g(ctx);
    const Ssb s(ctx, self);
    if (!s.valid("strstreambuf::pbackfail")) return kEof;
    const Sb<1> sb = s.sb();
    if (sb.gptr() == 0 || sb.gptr() <= sb.eback() ||
        (meta != kEof && static_cast<u8>(meta) != g.r8(sb.gptr() - 1) && (s.r(kSsStrmode) & kSsConstant)))
        return kEof;
    sb.gbump(-1);
    if (meta == kEof) return 0;
    g.w8(sb.gptr(), static_cast<u8>(meta));
    return meta & 0xFF;
}

// underflow(): the next character, the read area grown to the high-water mark when writing has gone past it.
s32 ssb_underflow_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    const Ssb s(ctx, self);
    if (!s.valid("strstreambuf::underflow")) return kEof;
    const Sb<1> sb = s.sb();
    if (sb.gptr() == 0) return kEof;
    if (sb.gptr() < sb.egptr()) return g.r8(sb.gptr());
    if (sb.pptr() == 0 || (sb.pptr() <= sb.gptr() && s.r(kSsSeekhigh) <= sb.gptr())) return kEof;
    if (s.r(kSsSeekhigh) < sb.pptr()) s.w(kSsSeekhigh, sb.pptr());
    sb.setg(sb.eback(), sb.gptr(), s.r(kSsSeekhigh));
    return g.r8(sb.gptr());
}

// seekoff(streamoff, seekdir, openmode) and seekpos(streampos, openmode): positions within [0, high-water].
inline s32 ssb_seek(const Ssb& s, s32 off, s32 way, u32 which, bool absolute)
{
    const Sb<1> sb = s.sb();
    if (sb.pptr() != 0 && s.r(kSsSeekhigh) < sb.pptr()) s.w(kSsSeekhigh, sb.pptr());
    const s32 high = static_cast<s32>(s.r(kSsSeekhigh) - sb.eback());
    if (absolute && off == kBadoff) return kBadoff;
    if ((which & kIn) && sb.gptr() != 0) {
        if (!absolute) {
            if (way == 2) off += high;
            else if (way == 1 && !(which & kOut)) off += static_cast<s32>(sb.gptr() - sb.eback());
            else if (way != 0 || off == kBadoff) off = kBadoff;
        }
        if (0 <= off && off <= high) {
            sb.gbump(static_cast<s32>(sb.eback() - sb.gptr()) + off);
            if ((which & kOut) && sb.pptr() != 0) sb.setp(sb.pbase(), sb.gptr(), sb.epptr());
        } else {
            off = kBadoff;
        }
    } else if ((which & kOut) && sb.pptr() != 0) {
        if (!absolute) {
            if (way == 2) off += high;
            else if (way == 1) off += static_cast<s32>(sb.pptr() - sb.eback());
            else if (way != 0 || off == kBadoff) off = kBadoff;
        }
        if (0 <= off && off <= high) sb.pbump(static_cast<s32>(sb.eback() - sb.pptr()) + off);
        else off = kBadoff;
    } else {
        off = kBadoff;
    }
    return off;
}

void ssb_seekoff_(PPUContext& ctx, u32 out, u32 self, s32 off, s32 way, s32 which)
{
    G g(ctx);
    if (!g.need("strstreambuf::seekoff", out, 24)) return;
    const Ssb s(ctx, self);
    if (!s.valid("strstreambuf::seekoff")) return pos_off(g, out, kBadoff);
    pos_off(g, out, ssb_seek(s, off, way, static_cast<u32>(which), false));
}

void ssb_seekpos_(PPUContext& ctx, u32 out, u32 self, u64 p0, u64 p1, u64 p2, s32 which)
{
    G g(ctx);
    if (!g.need("strstreambuf::seekpos", out, 24)) return;
    const Ssb s(ctx, self);
    if (!s.valid("strstreambuf::seekpos")) return pos_off(g, out, kBadoff);
    pos_off(g, out, ssb_seek(s, Pos(p0, p1, p2).offset(), 0, static_cast<u32>(which), true));
}

// ------------------------------------------------------------------------------------------------- the streams

// Where a stream class keeps its basic_ios (in a complete object) and its vtables' names.
struct StreamClass {
    std::string_view cls;
    u32 ios;     // offset of the virtual base in a complete object
    u32 member;  // offset of its strstreambuf (0: none)
    bool wide;
    u32 points;  // address points: [primary, (ostream-in-iostream), basic_ios]
};

inline constexpr StreamClass kIstreamC{"Si", 8, 0, false, 2}, kOstreamC{"So", 4, 0, false, 2},
    kIostreamC{"Sd", 12, 0, false, 3}, kIstreamW{"St13basic_istreamIwSt11char_traitsIwEE", 8, 0, true, 2},
    kOstreamW{"St13basic_ostreamIwSt11char_traitsIwEE", 4, 0, true, 2}, kIstrstream{"St10istrstream", 92, 8, false, 2},
    kOstrstream{"St10ostrstream", 88, 4, false, 2}, kStrstream{"St9strstream", 96, 12, false, 3};

// The vptrs of a complete object.
inline void stream_vptrs(PPUContext& ctx, u32 self, const StreamClass& c)
{
    G g(ctx);
    g.w32(self, vtable(ctx, c.cls, 0));
    if (c.points == 3) g.w32(self + 8, vtable(ctx, c.cls, 1));
    g.w32(self + c.ios, vtable(ctx, c.cls, c.points - 1));
}

inline u32 stream_bytes(const StreamClass& c) { return c.ios + kBasicIosBytes; }

// The complete-object destructor (D1): the vptrs, the member stream buffer, then the virtual base.
inline void stream_destroy(PPUContext& ctx, u32 self, const StreamClass& c)
{
    stream_vptrs(ctx, self, c);
    if (c.member) strstreambuf_destroy(ctx, self + c.member);
    if (c.wide) basic_ios_destroy<4>(ctx, self + c.ios);
    else basic_ios_destroy<1>(ctx, self + c.ios);
}

template <const StreamClass& C, bool Deleting>
void stream_dtor_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (!g.need("stream destructor", self, stream_bytes(C))) return;
    stream_destroy(ctx, self, C);
    if (Deleting) op_delete(ctx, self);
}

// The base-object destructor (D2, given the derived class's VTT): the member only. The vptrs are left to the derived
// class, which destroys the virtual base itself; nothing virtual runs here.
template <const StreamClass& C>
void stream_dtor_base_(PPUContext& ctx, u32 self, u32 /*vtt*/)
{
    G g(ctx);
    if (!g.need("stream destructor", self, C.member + kSsBytes)) return;
    if (C.member) strstreambuf_destroy(ctx, self + C.member);
}

// A virtual thunk: `this` adjusted by the vcall offset at -12 in its vtable, then the destructor.
template <void (*Fn)(PPUContext&, u32)>
void vthunk_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (!g.need("virtual thunk", self, 4) || !g.need("virtual thunk", g.r32(self) - 12, 4)) return;
    Fn(ctx, self + g.r32(g.r32(self) - 12));
}

// A non-virtual thunk from the ostream-in-iostream subobject (at +8).
template <void (*Fn)(PPUContext&, u32)>
void nvthunk8_(PPUContext& ctx, u32 self)
{
    Fn(ctx, self - 8);
}

// ostrstream(char *, int, openmode) and strstream(char *, int, openmode): the stream on a strstreambuf over the
// array, writing from its end for app (from its start otherwise).
inline u32 strstream_putptr(PPUContext& ctx, u32 ptr, u32 mode)
{
    if (ptr == 0 || (mode & kApp) == 0) return ptr;
    return ptr + static_cast<u32>(libc::guest_strnlen(G(ctx).t, ptr, libc::kNoLimit).value_or(0));
}

template <const StreamClass& C>
void strstream_ctor_(PPUContext& ctx, u32 self, u32 ptr, s32 count, u32 mode)
{
    G g(ctx);
    if (!g.need("strstream constructor", self, stream_bytes(C))) return;
    const u32 ios = self + C.ios;
    basic_ios_construct<1>(ctx, ios);
    if (C.points == 3) g.w32(self + 4, 0); // istream's _Chcount
    basic_ios_init<1>(ctx, ios, self + C.member, false);
    strstreambuf_construct(ctx, self + C.member, ptr, count, strstream_putptr(ctx, ptr, mode));
    stream_vptrs(ctx, self, C);
}

// The base-object constructor (C2, given the derived class's VTT): the virtual base, which the derived class made,
// is found through the VTT's first vtable (its virtual base offset at -12); the vptrs are left to the derived class.
template <const StreamClass& C>
void strstream_ctor_base_(PPUContext& ctx, u32 self, u32 vtt, u32 ptr, s32 count, u32 mode)
{
    G g(ctx);
    if (!g.need("strstream constructor", self, C.member + kSsBytes) || !g.need("strstream constructor", vtt, 4) ||
        !g.need("strstream constructor", g.r32(vtt) - 12, 4))
        return;
    const u32 ios = self + g.r32(g.r32(vtt) - 12);
    if (C.points == 3) g.w32(self + 4, 0);
    basic_ios_init<1>(ctx, ios, self + C.member, false);
    strstreambuf_construct(ctx, self + C.member, ptr, count, strstream_putptr(ctx, ptr, mode));
}

// ------------------------------------------------------------------------------------------------- the standard streams

// istream(&buf, true) and ostream(&buf, true) as Init and _Winit make them.
template <u32 E>
void make_istream(PPUContext& ctx, u32 self, u32 sb)
{
    const StreamClass& c = E == 1 ? kIstreamC : kIstreamW;
    basic_ios_construct<E>(ctx, self + c.ios);
    G(ctx).w32(self + 4, 0);
    stream_vptrs(ctx, self, c);
    basic_ios_init<E>(ctx, self + c.ios, sb, true);
}

template <u32 E>
void make_ostream(PPUContext& ctx, u32 self, u32 sb)
{
    const StreamClass& c = E == 1 ? kOstreamC : kOstreamW;
    basic_ios_construct<E>(ctx, self + c.ios);
    stream_vptrs(ctx, self, c);
    basic_ios_init<E>(ctx, self + c.ios, sb, true);
}

inline u32 stream_object(std::string_view name)
{
    const u32 a = HleProcess::current()->link_variable(kModule, recomp::nid_of(name));
    if (!a) PS_FATAL(kChan, "no memory for {}", name);
    return a;
}

// The streams of Init (cin, cout, cerr, clog) and _Winit (wcin...): filebufs over stdin, stdout and stderr; cin and
// cerr and clog tied to cout; cerr unit-buffered.
template <u32 E>
void make_std_streams(PPUContext& ctx)
{
    G g(ctx);
    const bool w = E == 4;
    const u32 fin = static_object(w ? "wfin" : "fin", FbLayout<E>::kBytes),
              fout = static_object(w ? "wfout" : "fout", FbLayout<E>::kBytes),
              ferr = static_object(w ? "wferr" : "ferr", FbLayout<E>::kBytes);
    filebuf_construct<E>(ctx, fin, libc_variable("_Stdin"));
    filebuf_construct<E>(ctx, fout, libc_variable("_Stdout"));
    const u32 in = stream_object(w ? "_ZSt4wcin" : "_ZSt3cin"), out = stream_object(w ? "_ZSt5wcout" : "_ZSt4cout"),
              err = stream_object(w ? "_ZSt5wcerr" : "_ZSt4cerr"), log = stream_object(w ? "_ZSt5wclog" : "_ZSt4clog");
    make_istream<E>(ctx, in, fin);
    make_ostream<E>(ctx, out, fout);
    g.w32(in + 8 + kIosTie, out);
    filebuf_construct<E>(ctx, ferr, libc_variable("_Stderr"));
    make_ostream<E>(ctx, err, ferr);
    g.w32(err + 4 + kIosTie, out);
    g.w32(err + 4 + kIosFmtfl, g.r32(err + 4 + kIosFmtfl) | kUnitbuf);
    make_ostream<E>(ctx, log, ferr);
    g.w32(log + 4 + kIosTie, out);
}

// ostream::flush(): the buffer's sync when the stream is good; badbit when it fails.
inline void ostream_flush(PPUContext& ctx, u32 os)
{
    G g(ctx);
    const u32 ios = os + 4;
    if (g.r32(ios + kIosState) & (kFailbit | kBadbit)) return;
    const u32 sb = g.r32(ios + kIosSb);
    if (sb && v_sync(ctx, sb) == -1) ios_setstate(ctx, ios, kBadbit);
}

// Init(): the first one makes the standard streams; the count goes up.
template <u32 E>
void std_init(PPUContext& ctx)
{
    {
        Lockit lock(ctx, kLockStream);
        StdStreams& s = std_streams();
        std::lock_guard l(s.mutex);
        s32& cnt = E == 1 ? s.init_cnt : s.winit_cnt;
        if (0 <= cnt) {
            ++cnt;
            return;
        }
        cnt = 1;
    }
    make_std_streams<E>(ctx);
}

// ~Init(): the last one flushes cout, cerr and clog.
template <u32 E>
void std_fini(PPUContext& ctx)
{
    bool flush = false;
    {
        Lockit lock(ctx, kLockStream);
        StdStreams& s = std_streams();
        std::lock_guard l(s.mutex);
        s32& cnt = E == 1 ? s.init_cnt : s.winit_cnt;
        flush = --cnt == 0;
    }
    if (!flush) return;
    const bool w = E == 4;
    for (const char* n : {w ? "_ZSt5wcout" : "_ZSt4cout", w ? "_ZSt5wcerr" : "_ZSt4cerr", w ? "_ZSt5wclog" : "_ZSt4clog"})
        ostream_flush(ctx, stream_object(n));
}

template <u32 E>
void std_init_(PPUContext& ctx, u32)
{
    std_init<E>(ctx);
}
template <u32 E>
void std_fini_(PPUContext& ctx, u32)
{
    std_fini<E>(ctx);
}

// ------------------------------------------------------------------------------------------------- iterators

// istreambuf_iterator {_Strbuf, _Got (+4), _Val (+5 or +8)}.
template <u32 E>
inline constexpr u32 kItVal = E == 1 ? 5 : 8;

// _Peek(): the next element (no stream buffer after the end).
template <u32 E>
u32 it_peek(PPUContext& ctx, u32 self)
{
    G g(ctx);
    const u32 sb = g.r32(self);
    s32 meta = kEof;
    if (sb == 0 || (meta = sgetc<E>(ctx, sb)) == kEof) g.w32(self, 0);
    else g.template wel<E>(self + kItVal<E>, static_cast<u32>(meta));
    g.w8(self + 4, 1);
    return g.template rel<E>(self + kItVal<E>);
}

template <u32 E>
u32 it_peek_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    return g.need("istreambuf_iterator::_Peek", self, kItVal<E> + E) ? it_peek<E>(ctx, self) : 0;
}

// _Inc(): past the element.
template <u32 E>
void it_inc_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (!g.need("istreambuf_iterator::_Inc", self, kItVal<E> + E)) return;
    const u32 sb = g.r32(self);
    if (sb == 0 || sbumpc<E>(ctx, sb) == kEof) {
        g.w32(self, 0);
        g.w8(self + 4, 1);
    } else {
        g.w8(self + 4, 0);
    }
}

// operator*() const: the element, peeked first when not yet got.
template <u32 E>
u32 it_deref_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (!g.need("istreambuf_iterator::operator*", self, kItVal<E> + E)) return 0;
    if (!g.r8(self + 4)) it_peek<E>(ctx, self);
    return g.template rel<E>(self + kItVal<E>);
}

// ostreambuf_iterator {_Failed, _Strbuf (+4)}: operator=(_Elem) puts the element; a failure is remembered.
template <u32 E>
u32 oit_assign_(PPUContext& ctx, u32 self, u32 ch)
{
    G g(ctx);
    if (!g.need("ostreambuf_iterator::operator=", self, 8)) return self;
    const u32 sb = g.r32(self + 4);
    if (sb == 0 || sputc<E>(ctx, sb, ch) == kEof) g.w8(self, 1);
    return self;
}

// ------------------------------------------------------------------------------------------------- manipulators

// _Smanip<int> {_Pfun, _Manarg} with the library's (internal) functions.
inline void smanip(PPUContext& ctx, u32 out, std::string_view fn, s32 arg)
{
    G g(ctx);
    if (!g.need("manipulator", out, 8)) return;
    g.w32(out, descriptor(fn));
    g.w32(out + 4, static_cast<u32>(arg));
}

inline void ios_setf(PPUContext& ctx, u32 ios, u32 fl, u32 mask)
{
    G g(ctx);
    if (!g.need("manipulator", ios, kIosBaseBytes)) return;
    const u32 old = g.r32(ios + kIosFmtfl);
    g.w32(ios + kIosFmtfl, (old & ~mask) | (fl & mask & kFmtmask));
}

void swfun_(PPUContext& ctx, u32 ios, s32 n)
{
    G g(ctx);
    if (g.need("setw", ios, kIosBaseBytes)) g.w32(ios + kIosWide, static_cast<u32>(n));
}
void spfun_(PPUContext& ctx, u32 ios, s32 n)
{
    G g(ctx);
    if (g.need("setprecision", ios, kIosBaseBytes)) g.w32(ios + kIosPrec, static_cast<u32>(n));
}
void sbfun_(PPUContext& ctx, u32 ios, s32 base)
{
    ios_setf(ctx, ios, base == 8 ? kOct : base == 10 ? kDec : base == 16 ? kHex : 0, kBasefield);
}
void sifun_(PPUContext& ctx, u32 ios, s32 mask) { ios_setf(ctx, ios, static_cast<u32>(mask), static_cast<u32>(mask)); }
void rsfun_(PPUContext& ctx, u32 ios, s32 mask) { ios_setf(ctx, ios, 0, static_cast<u32>(mask)); }

inline constexpr std::string_view kSwfun = "_pspocket_stdcxx_swfun", kSpfun = "_pspocket_stdcxx_spfun",
                                  kSbfun = "_pspocket_stdcxx_sbfun", kSifun = "_pspocket_stdcxx_sifun",
                                  kRsfun = "_pspocket_stdcxx_rsfun";

void setw_(PPUContext& ctx, u32 out, s32 n) { smanip(ctx, out, kSwfun, n); }
void setprecision_(PPUContext& ctx, u32 out, s32 n) { smanip(ctx, out, kSpfun, n); }
void setbase_(PPUContext& ctx, u32 out, s32 n) { smanip(ctx, out, kSbfun, n); }
void setiosflags_(PPUContext& ctx, u32 out, s32 mask) { smanip(ctx, out, kSifun, mask); }
void resetiosflags_(PPUContext& ctx, u32 out, s32 mask) { smanip(ctx, out, kRsfun, mask); }

} // namespace pspocket::hle::stdcxx
