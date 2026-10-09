#pragma once

// sys_libstdcxx, the foundation (D-560): operator new and delete, the exception classes and their raising, the
// terminate/unexpected/new handlers, _Debug_message, _Mutex and the strings' out-of-line members. Included by
// stdcxx_streams.cpp only.

#include "stdcxx.hpp"

namespace pspocket::hle::stdcxx {

// The library's handlers (set_new_handler, set_terminate, set_unexpected, exception::_Set_raise_handler): descriptors.
struct Handlers {
    std::mutex mutex;
    u32 new_handler = 0;
    u32 terminate = 0;  // 0 until set: the default (abort), which set_terminate hands back as a descriptor
    u32 unexpected = 0; // 0 until set: the default (terminate)
    u32 raise = 0;
};

inline Handlers& handlers() { return HleProcess::current()->state<Handlers>(); }

// ------------------------------------------------------------------------------------------------- new and delete

// operator new(size_t): sys_libc's malloc, the new handler called while it fails, bad_alloc raised when there is no
// handler (the SDK's: malloc, then the handler, then bad_alloc). malloc(0) gives a block, as dlmalloc does.
inline u32 new_loop(PPUContext& ctx, u32 size, u32 align, bool nothrow)
{
    for (;;) {
        const u32 p = align ? static_cast<u32>(libc_fn::memalign(ctx, {align, size}))
                            : static_cast<u32>(libc_fn::malloc(ctx, {size}));
        if (p) return p;
        u32 h = 0;
        {
            Handlers& s = handlers();
            std::lock_guard lock(s.mutex);
            h = s.new_handler;
        }
        if (!h) {
            if (nothrow) return 0;
            raise_new(ctx, "St9bad_alloc");
        }
        call_desc(ctx, h, {});
    }
}

u32 op_new(PPUContext& ctx, u32 size) { return new_loop(ctx, size, 0, false); }

void op_delete(PPUContext& ctx, u32 p)
{
    if (p) libc_fn::free(ctx, {p});
}

u32 op_new_(PPUContext& ctx, u32 size) { return new_loop(ctx, size, 0, false); }
u32 op_new_nothrow_(PPUContext& ctx, u32 size, u32) { return new_loop(ctx, size, 0, true); }
// Sony's aligned forms: operator new(size_t size, size_t alignment), memalign's.
u32 op_new_aligned_(PPUContext& ctx, u32 size, u32 align) { return new_loop(ctx, size, align ? align : 1, false); }
u32 op_new_aligned_nothrow_(PPUContext& ctx, u32 size, u32 align, u32)
{
    return new_loop(ctx, size, align ? align : 1, true);
}
void op_delete_(PPUContext& ctx, u32 p) { op_delete(ctx, p); }
void op_delete2_(PPUContext& ctx, u32 p, u32) { op_delete(ctx, p); }
void op_delete3_(PPUContext& ctx, u32 p, u32, u32) { op_delete(ctx, p); }
// The placement forms (a trailing void*): nothing to free.
void op_delete_placement_(PPUContext&, u32, u32) {}
void op_delete_placement3_(PPUContext&, u32, u32, u32) {}

// ------------------------------------------------------------------------------------------------- exceptions

// Every exception class's object: {vptr} for exception, bad_alloc and bad_cast (4 bytes); {vptr, string _Str} for
// logic_error, length_error, out_of_range, runtime_error and ios_base::failure (32 bytes). Their vtables: [~D1, ~D0,
// what, _Doraise].
inline constexpr u32 kExcStr = 4, kExcBytes = 32;

inline bool has_string(std::string_view cls)
{
    return cls == "St11logic_error" || cls == "St12length_error" || cls == "St12out_of_range" ||
        cls == "St13runtime_error" || cls == "NSt8ios_base7failureE";
}

// _Throw(const exception&): Dinkumware's, as the SDK's: "exception: " and what() (or "unknown") on stderr, then abort.
[[noreturn]] void throw_(PPUContext& ctx, u32 exc)
{
    const u32 what = static_cast<u32>(vcall(ctx, exc, 2, {exc}));
    G g(ctx);
    std::string text = "unknown";
    if (what) {
        if (auto s = g.str(what)) text = *s;
    }
    const u32 err = libc_variable("_Stderr");
    libc_fn::fputs(ctx, {cstring(ctx, "exception: "), err});
    libc_fn::fputs(ctx, {what ? what : cstring(ctx, "unknown"), err});
    libc_fn::fputs(ctx, {cstring(ctx, "\n"), err});
    PS_LOG_ERROR(kChan, "C++ exception raised with no handler to take it: \"{}\" (the program aborts)", text);
    libc_fn::abort(ctx, {});
    PS_FATAL(kChan, "abort() returned");
}

void raise_object(PPUContext& ctx, u32 exc)
{
    u32 h = 0;
    {
        Handlers& s = handlers();
        std::lock_guard lock(s.mutex);
        h = s.raise;
    }
    if (h) call_desc(ctx, h, {exc});
    vcall(ctx, exc, 3, {exc}); // _Doraise
    throw_(ctx, exc);
}

void raise_new(PPUContext& ctx, std::string_view cls, std::string_view message)
{
    G g(ctx);
    StackVar obj(ctx, g.b, kExcBytes);
    g.w32(obj.addr(), vtable(ctx, cls));
    if (has_string(cls)) {
        GStr<1> s(g, obj.addr() + kExcStr);
        s.construct();
        s.set(message);
    }
    raise_object(ctx, obj.addr());
}

void xlen(PPUContext& ctx) { raise_new(ctx, "St12length_error", "string too long"); }
void xran(PPUContext& ctx) { raise_new(ctx, "St12out_of_range", "invalid string position"); }

// what(): exception's is "" (the SDK's), bad_alloc's and bad_cast's their fixed text, the others their string's.
u32 exception_what_(PPUContext& ctx, u32) { return cstring(ctx, ""); }
u32 bad_alloc_what_(PPUContext& ctx, u32) { return cstring(ctx, "bad allocation"); }
u32 bad_cast_what_(PPUContext& ctx, u32) { return cstring(ctx, "bad cast"); }
u32 string_what_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (!g.need("what", self, kExcBytes)) return 0;
    return GStr<1>(g, self + kExcStr).ptr();
}

// _Doraise: exception's does nothing; the others raise themselves (_RAISE(*this): _Throw in the no-exception library).
void exception_doraise_(PPUContext&, u32) {}
void doraise_(PPUContext& ctx, u32 self) { throw_(ctx, self); }

void exception_raise_(PPUContext& ctx, u32 self) { raise_object(ctx, self); }

u32 set_raise_handler_(PPUContext& ctx, u32 fn)
{
    (void)ctx;
    Handlers& s = handlers();
    std::lock_guard lock(s.mutex);
    const u32 old = s.raise;
    s.raise = fn;
    return old;
}

void _Throw_(PPUContext& ctx, u32 exc) { throw_(ctx, exc); }

// The destructors: D1 and D2 reset the vptr to their class's and destroy the members, then the base's; D0 frees too.
inline void exception_dtor(PPUContext& ctx, u32 self, std::string_view cls, bool deleting)
{
    G g(ctx);
    if (!g.need("~exception", self, has_string(cls) ? kExcBytes : 4)) return;
    g.w32(self, vtable(ctx, cls));
    if (has_string(cls)) GStr<1>(g, self + kExcStr).destroy();
    g.w32(self, vtable(ctx, "St9exception"));
    if (deleting) op_delete(ctx, self);
}

template <const char* Cls, bool Deleting>
void exc_dtor_(PPUContext& ctx, u32 self)
{
    exception_dtor(ctx, self, Cls, Deleting);
}

inline constexpr char kException[] = "St9exception", kBadAlloc[] = "St9bad_alloc", kBadCast[] = "St8bad_cast",
                      kLogicError[] = "St11logic_error", kLengthError[] = "St12length_error",
                      kOutOfRange[] = "St12out_of_range", kRuntimeError[] = "St13runtime_error",
                      kFailure[] = "NSt8ios_base7failureE";

// bad_cast(): its vtable.
void bad_cast_ctor_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (g.need("bad_cast::bad_cast", self, 4)) g.w32(self, vtable(ctx, kBadCast));
}

// _String_base::_Xlen() and _Xran() (static in effect: their `this` is not used).
void xlen_(PPUContext& ctx, u32) { xlen(ctx); }
void xran_(PPUContext& ctx, u32) { xran(ctx); }

// ------------------------------------------------------------------------------------------------- handlers

// The defaults set_terminate and set_unexpected hand back the first time: the library's own (internal) functions.
inline constexpr std::string_view kDefaultTerminate = "_pspocket_stdcxx_default_terminate";
inline constexpr std::string_view kDefaultUnexpected = "_pspocket_stdcxx_default_unexpected";

void default_terminate_(PPUContext& ctx)
{
    log_call_site(ctx, guest_base(ctx), "std::terminate(): the program aborts");
    libc_fn::abort(ctx, {});
    PS_FATAL(kChan, "abort() returned");
}

// terminate(): the handler, then abort (a handler must not return; when it does the program ends all the same).
void terminate_(PPUContext& ctx)
{
    u32 h = 0;
    {
        Handlers& s = handlers();
        std::lock_guard lock(s.mutex);
        h = s.terminate;
    }
    if (h) call_desc(ctx, h, {});
    default_terminate_(ctx);
}

void default_unexpected_(PPUContext& ctx) { terminate_(ctx); }

// unexpected(): the handler (by default terminate).
void unexpected_(PPUContext& ctx)
{
    u32 h = 0;
    {
        Handlers& s = handlers();
        std::lock_guard lock(s.mutex);
        h = s.unexpected;
    }
    if (h) call_desc(ctx, h, {});
    else terminate_(ctx);
    // An unexpected handler that returns ends the program, as a throw it cannot make would.
    terminate_(ctx);
}

u32 set_terminate_(PPUContext&, u32 fn)
{
    Handlers& s = handlers();
    std::unique_lock lock(s.mutex);
    const u32 old = s.terminate;
    s.terminate = fn;
    lock.unlock();
    return old ? old : descriptor(kDefaultTerminate);
}

u32 set_unexpected_(PPUContext&, u32 fn)
{
    Handlers& s = handlers();
    std::unique_lock lock(s.mutex);
    const u32 old = s.unexpected;
    s.unexpected = fn;
    lock.unlock();
    return old ? old : descriptor(kDefaultUnexpected);
}

u32 set_new_handler_(PPUContext&, u32 fn)
{
    Handlers& s = handlers();
    std::lock_guard lock(s.mutex);
    const u32 old = s.new_handler;
    s.new_handler = fn;
    return old;
}

// _Debug_message(message, file): the debug iterators' report, on stderr as "<file>: <message>", then abort.
void _Debug_message_(PPUContext& ctx, u32 message, u32 file)
{
    G g(ctx);
    const u32 err = libc_variable("_Stderr");
    if (file && g.str(file)) {
        libc_fn::fputs(ctx, {file, err});
        libc_fn::fputs(ctx, {cstring(ctx, ": "), err});
    }
    if (message && g.str(message)) libc_fn::fputs(ctx, {message, err});
    libc_fn::fputs(ctx, {cstring(ctx, "\n"), err});
    log_call_site(ctx, g.b, std::format("_Debug_message(\"{}\", \"{}\"): the program aborts",
        message ? g.str(message).value_or("?") : "", file ? g.str(file).value_or("?") : ""));
    libc_fn::abort(ctx, {});
    PS_FATAL(kChan, "abort() returned");
}

// ------------------------------------------------------------------------------------------------- _Mutex

// _Mutex {void *_Mtx}: a 40-byte block from operator new (the C library's lock object, as the SDK's) made a lock by
// sys_libc's _Mtxinit; _Lock and _Unlock are _Mtxlock and _Mtxunlock on it.
inline constexpr u32 kRmtxBytes = 40;

void mutex_ctor_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (!g.need("_Mutex::_Mutex", self, 4)) return;
    const u32 m = op_new(ctx, kRmtxBytes);
    g.w32(self, m);
    libc_fn::mtxinit(ctx, {m});
}

void mutex_dtor_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (!g.need("_Mutex::~_Mutex", self, 4)) return;
    const u32 m = g.r32(self);
    if (m) {
        libc_fn::mtxdst(ctx, {m});
        op_delete(ctx, m);
    }
}

void mutex_lock_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (g.need("_Mutex::_Lock", self, 4)) libc_fn::mtxlock(ctx, {g.r32(self)});
}

void mutex_unlock_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (g.need("_Mutex::_Unlock", self, 4)) libc_fn::mtxunlock(ctx, {g.r32(self)});
}

// ------------------------------------------------------------------------------------------------- strings

template <u32 E>
bool str_ok(G& g, std::string_view fn, u32 self)
{
    using L = StrLayout<E>;
    if (!g.need(fn, self, L::kBytes)) return false;
    const GStr<E> s(g, self);
    if (s.res() >= L::kBuf && !g.need(fn, s.ptr(), (u64{s.res()} + 1) * E)) return false;
    return true;
}

template <u32 E>
u32 str_ctor_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (g.need("basic_string::basic_string", self, StrLayout<E>::kBytes)) GStr<E>(g, self).construct();
    return self;
}

// basic_string(const char *): _Tidy(), then assign(_Ptr, traits::length(_Ptr)).
u32 str_ctor_cstr_(PPUContext& ctx, u32 self, u32 src)
{
    G g(ctx);
    if (!g.need("basic_string::basic_string", self, StrLayout<1>::kBytes)) return self;
    const GStr<1> s(g, self);
    s.construct();
    const auto len = libc::guest_strnlen(g.t, src, libc::kNoLimit);
    if (!src || !len) {
        log_call_site(ctx, g.b, std::format("basic_string(const char *{:#x}): not a string", src));
        return self;
    }
    std::vector<u8> v(g.b + src, g.b + src + *len);
    s.assign_bytes(v, static_cast<u32>(*len));
    return self;
}

// basic_string(const basic_string&): _Tidy(), then assign(_Right, 0, npos).
u32 str_ctor_copy_(PPUContext& ctx, u32 self, u32 right)
{
    G g(ctx);
    if (!g.need("basic_string::basic_string", self, StrLayout<1>::kBytes) || !str_ok<1>(g, "basic_string", right))
        return self;
    const GStr<1> s(g, self);
    s.construct();
    s.assign(right, 0, npos);
    return self;
}

void str_dtor_(PPUContext& ctx, u32 self)
{
    G g(ctx);
    if (str_ok<1>(g, "basic_string::~basic_string", self)) GStr<1>(g, self).destroy();
}

template <u32 E>
u32 str_grow_(PPUContext& ctx, u32 self, u32 newsize, bool trim)
{
    G g(ctx);
    if (!str_ok<E>(g, "basic_string::_Grow", self)) return 0;
    return GStr<E>(g, self).grow(newsize, trim) ? 1 : 0;
}

template <u32 E>
void str_tidy_(PPUContext& ctx, u32 self, bool built, u32 newsize)
{
    G g(ctx);
    if (str_ok<E>(g, "basic_string::_Tidy", self)) GStr<E>(g, self).tidy(built, newsize);
}

template <u32 E>
u32 str_erase_(PPUContext& ctx, u32 self, u32 off, u32 count)
{
    G g(ctx);
    if (str_ok<E>(g, "basic_string::erase", self)) GStr<E>(g, self).erase(off, count);
    return self;
}

template <u32 E>
u32 str_append_(PPUContext& ctx, u32 self, u32 count, u32 ch)
{
    G g(ctx);
    if (str_ok<E>(g, "basic_string::append", self)) GStr<E>(g, self).append(count, E == 1 ? (ch & 0xFF) : ch);
    return self;
}

template <u32 E>
u32 str_assign_(PPUContext& ctx, u32 self, u32 right, u32 roff, u32 count)
{
    G g(ctx);
    if (str_ok<E>(g, "basic_string::assign", self) && str_ok<E>(g, "basic_string::assign", right))
        GStr<E>(g, self).assign(right, roff, count);
    return self;
}

template <u32 E>
u32 str_insert_(PPUContext& ctx, u32 self, u32 off, u32 count, u32 ch)
{
    G g(ctx);
    if (str_ok<E>(g, "basic_string::insert", self)) GStr<E>(g, self).insert(off, count, E == 1 ? (ch & 0xFF) : ch);
    return self;
}

template <u32 E>
u32 str_replace_(PPUContext& ctx, u32 self, u32 off, u32 n0, u32 src, u32 count)
{
    G g(ctx);
    if (!str_ok<E>(g, "basic_string::replace", self)) return self;
    if (count && !g.need("basic_string::replace", src, u64{count} * E)) return self;
    GStr<E>(g, self).replace(off, n0, src, count);
    return self;
}

} // namespace pspocket::hle::stdcxx
