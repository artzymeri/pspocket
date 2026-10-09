#pragma once

// sys_libstdcxx's object model (D-560): how pspocket's own C++ library lays the SDK's objects out in guest memory,
// makes their vtables and type_info, calls virtual functions, raises exceptions and reaches the libraries it stands on.
// Included by stdcxx_streams.cpp only, before the parts (stdcxx_core.hpp, stdcxx_locale.hpp, stdcxx_ios.hpp).
//
// The SDK's library is Dinkumware's of VC8's generation (2005) built by ppu-lv2-gcc for the PPU's lv2 ABI, with the
// exception classes of its no-exception configuration (_Raise, _Doraise, _Throw). The ABI throughout:
// - Itanium C++ ABI as GCC builds it for lv2: ILP32 on 64-bit registers (pointers, size_t, long 4 bytes), big-endian,
//   wchar_t a 32-bit big-endian word (as sys_libc's, D-512), long double = double.
// - A vtable's entries are 4-byte pointers to function descriptors (entry, TOC); offset-to-top and the type_info
//   pointer sit before the address point, a class with virtual bases has its virtual base offset before them and the
//   vcall offsets of the secondary vtables before those (the virtual thunks read the destructor's at -12).
// - Arguments and results as the PPC64 ELF ABI passes them: `this` in r3, an aggregate result (locale, pos_type,
//   _Smanip, _Cvtvec) in memory through r3 with `this` in r4; an aggregate argument (pos_type) in registers as its
//   doublewords.
// - Objects are made with the library's operator new (sys_libc's malloc) and destroyed through their vtables' deleting
//   destructor (slot 1), as the SDK's code does; a virtual function whose descriptor is one of pspocket's runs natively
//   without the stub, any other through call_guest.
//
// The parts of the library meet only through the registry: a function of the facets part (D-561) is called by its
// mangled name (Callee), and a missing one is logged with its name and raises bad_cast, as the SDK's use_facet does for
// a facet it cannot make.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "guest.hpp"
#include "libc_guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle::stdcxx {

inline constexpr std::string_view kModule = "sys_libstdcxx";
inline constexpr std::string_view kChan = "sys_libstdcxx";
inline constexpr u32 npos = 0xFFFFFFFFu;

// ------------------------------------------------------------------------------------------------- guest access

// The calling thread's view of guest memory, with the checks of D-042.
struct G {
    PPUContext& ctx;
    lv2::PpuThread& t;
    u8* b;

    explicit G(PPUContext& c) : ctx(c), t(thread_of(c)), b(t.kernel.base()) {}

    u8 r8(u32 a) const { return ps_read8(b, a); }
    u16 r16(u32 a) const { return ps_read16(b, a); }
    u32 r32(u32 a) const { return ps_read32(b, a); }
    u64 r64(u32 a) const { return ps_read64(b, a); }
    void w8(u32 a, u8 v) const { ps_write8(b, a, v); }
    void w16(u32 a, u16 v) const { ps_write16(b, a, v); }
    void w32(u32 a, u32 v) const { ps_write32(b, a, v); }
    void w64(u32 a, u64 v) const { ps_write64(b, a, v); }
    // An element of a string: a byte (E = 1) or a big-endian wchar_t (E = 4).
    template <u32 E>
    u32 rel(u32 a) const
    {
        if constexpr (E == 1) return r8(a);
        else return r32(a);
    }
    template <u32 E>
    void wel(u32 a, u32 v) const
    {
        if constexpr (E == 1) w8(a, static_cast<u8>(v));
        else w32(a, v);
    }

    // Whether [a, a + n) is committed guest memory.
    bool ok(u32 a, u64 n) const { return libc::guest_range(t, a, n); }
    // The same, logged with the call site when it is not (`fn` names the function refusing).
    bool need(std::string_view fn, u32 a, u64 n) const
    {
        if (a && ok(a, n)) return true;
        log_call_site(ctx, b, std::format("{}: [{:#x}, +{:#x}) is not guest memory", fn, a, n));
        return false;
    }
    // A NUL-terminated string (nullopt when it runs into memory that isn't committed).
    std::optional<std::string> str(u32 a) const { return libc::read_string(t, a); }
};

// ------------------------------------------------------------------------------------------------- calls

// Calls the host function `f` as guest code would: arguments in r3..r10, the ninth and later in a parameter save area
// below r1 (as call_guest does); r3 back. The caller's r3..r12 and r1 are as they were afterwards.
inline u64 invoke(PPUContext& ctx, const Function& f, std::initializer_list<u64> args)
{
    std::array<u64, 10> saved{};
    for (u32 k = 0; k < 10; k++) saved[k] = ctx.gpr[3 + k];
    const u64 sp = ctx.gpr[1];
    const u32 extra = args.size() > 8 ? static_cast<u32>(args.size()) - 8 : 0;
    u8* b = guest_base(ctx);
    if (extra) {
        ctx.gpr[1] = (sp - 0x80 - 8 * extra) & ~u64{15};
        ps_write64(b, static_cast<u32>(ctx.gpr[1]), sp);
    }
    u32 i = 0;
    for (const u64 a : args) {
        if (i < 8) ctx.gpr[3 + i] = a;
        else ps_write64(b, static_cast<u32>(ctx.gpr[1]) + 0x70 + 8 * (i - 8), a);
        i++;
    }
    f.fn(ctx, b);
    const u64 r = ctx.gpr[3];
    ctx.gpr[1] = sp;
    for (u32 k = 0; k < 10; k++) ctx.gpr[3 + k] = saved[k];
    return r;
}

// A function of a library pspocket serves (this one's other part, sys_libc), found in the registry by name the first
// time it is called.
class Callee {
public:
    constexpr Callee(std::string_view module, std::string_view name) : module_(module), name_(name) {}

    std::string_view name() const { return name_; }
    const Function* find() const
    {
        if (!looked_.load(std::memory_order_acquire)) {
            f_.store(registry().find(module_, recomp::nid_of(name_)), std::memory_order_relaxed);
            looked_.store(true, std::memory_order_release);
        }
        return f_.load(std::memory_order_relaxed);
    }
    // The function, which must exist (a library pspocket always serves).
    const Function& get() const
    {
        const Function* f = find();
        if (!f) PS_FATAL(kChan, "{}::{} is not registered: sys_libstdcxx calls it", module_, name_);
        return *f;
    }
    u64 operator()(PPUContext& ctx, std::initializer_list<u64> args) const { return invoke(ctx, get(), args); }

private:
    std::string_view module_, name_;
    mutable std::atomic<const Function*> f_{nullptr};
    mutable std::atomic<bool> looked_{false};
};

// sys_libc's functions the library calls (the SDK's library is linked over the same C library).
namespace libc_fn {
inline const Callee malloc{"sys_libc", "malloc"}, free{"sys_libc", "free"}, realloc{"sys_libc", "realloc"},
    memalign{"sys_libc", "memalign"}, abort{"sys_libc", "abort"}, fputs{"sys_libc", "fputs"},
    fopen{"sys_libc", "fopen"}, fclose{"sys_libc", "fclose"}, fgetc{"sys_libc", "fgetc"},
    fputc{"sys_libc", "fputc"}, ungetc{"sys_libc", "ungetc"}, fread{"sys_libc", "fread"},
    fwrite{"sys_libc", "fwrite"}, fflush{"sys_libc", "fflush"}, fseek{"sys_libc", "fseek"},
    ftell{"sys_libc", "ftell"}, setvbuf{"sys_libc", "setvbuf"}, lockfile{"sys_libc", "_Lockfilelock"},
    unlockfile{"sys_libc", "_Unlockfilelock"}, mtxinit{"sys_libc", "_Mtxinit"}, mtxdst{"sys_libc", "_Mtxdst"},
    mtxlock{"sys_libc", "_Mtxlock"}, mtxunlock{"sys_libc", "_Mtxunlock"}, lock{"sys_libc", "_Locksyslock"},
    unlock{"sys_libc", "_Unlocksyslock"}, atexit{"sys_libc", "_Atexit"}, getpmbstate{"sys_libc", "_Getpmbstate"},
    getpwcstate{"sys_libc", "_Getpwcstate"}, getpcostate{"sys_libc", "_Getpcostate"},
    getpwcostate{"sys_libc", "_Getpwcostate"};
} // namespace libc_fn

// The guest address of a host variable of sys_libc (its FILEs, the ctype tables).
inline u32 libc_variable(std::string_view name)
{
    return HleProcess::current()->link_variable("sys_libc", recomp::nid_of(name));
}

// Dinkumware's numbered system locks (_Lockit): the C library's, so the game's inline code and this library agree.
enum LockKind : s32 { kLockLocale = 0, kLockMalloc = 1, kLockStream = 2, kLockDebug = 3 };

class Lockit {
public:
    Lockit(PPUContext& ctx, LockKind kind) : ctx_(ctx), kind_(kind) { libc_fn::lock(ctx_, {static_cast<u64>(kind_)}); }
    ~Lockit() { libc_fn::unlock(ctx_, {static_cast<u64>(kind_)}); }
    Lockit(const Lockit&) = delete;
    Lockit& operator=(const Lockit&) = delete;

private:
    PPUContext& ctx_;
    LockKind kind_;
};

// Calls the function whose descriptor is `desc`: natively when it is one of pspocket's (an HLE stub's descriptor),
// otherwise as guest code (call_guest). An address that isn't a descriptor is logged and answers `fallback`.
inline u64 call_desc(PPUContext& ctx, u32 desc, std::initializer_list<u64> args, u64 fallback = 0)
{
    G g(ctx);
    if (!desc || !g.ok(desc, 8)) {
        log_call_site(ctx, g.b, std::format("sys_libstdcxx: a call through {:#x}, which is no function descriptor", desc));
        return fallback;
    }
    const u32 entry = g.r32(desc);
    if (HleProcess* p = HleProcess::current()) {
        if (const LinkedFunction* lf = p->linked_at(entry); lf && lf->impl) return invoke(ctx, *lf->impl, args);
    }
    return call_guest(ctx, desc, args);
}

// The descriptor in slot `slot` of the vtable of the object at `obj` (0 when there is none to read).
inline u32 vslot(const G& g, u32 obj, u32 slot)
{
    if (!obj || !g.ok(obj, 4)) return 0;
    const u32 vptr = g.r32(obj);
    if (!vptr || !g.ok(vptr + 4 * slot, 4)) return 0;
    return g.r32(vptr + 4 * slot);
}

// A virtual call: `args` start with `this` (or with the result's address, then `this`, for an aggregate result).
inline u64 vcall(PPUContext& ctx, u32 obj, u32 slot, std::initializer_list<u64> args, u64 fallback = 0)
{
    G g(ctx);
    const u32 desc = vslot(g, obj, slot);
    if (!desc) {
        log_call_site(ctx, g.b, std::format("sys_libstdcxx: virtual call (slot {}) on {:#x}, which has no vtable", slot,
            obj));
        return fallback;
    }
    return call_desc(ctx, desc, args, fallback);
}

// ------------------------------------------------------------------------------------------------- operator new

// The library's operator new and delete (stdcxx_core.hpp): sys_libc's malloc with the new handler, as the SDK's.
u32 op_new(PPUContext& ctx, u32 size);
void op_delete(PPUContext& ctx, u32 p);

// ------------------------------------------------------------------------------------------------- vtables, type_info

// One word of a vtable: a value, a type_info (by its mangled type name) or a function's descriptor (by its mangled
// name, a function of the registry).
struct VWord {
    enum class Kind : u8 { Value, Type, Func };
    Kind kind;
    s32 value;
    std::string_view name;
};
inline VWord V(s32 v) { return {VWord::Kind::Value, v, {}}; }
inline VWord T(std::string_view type) { return {VWord::Kind::Type, 0, type}; }
inline VWord F(std::string_view fn) { return {VWord::Kind::Func, 0, fn}; }

struct VtableDef {
    std::string_view cls;          // the class's mangled type name ("St9exception")
    std::vector<VWord> words;
    std::vector<u32> points;       // the address points, as indexes into `words`
};

// type_info objects as GCC lays them out: __class_type_info {vptr, name}, __si_class_type_info {vptr, name, base},
// __vmi_class_type_info {vptr, name, flags, count, {base, offset_flags}...}. pspocket has none of libsupc++'s type_info
// classes, so their vptr is 0: typeid's name and comparisons work, dynamic_cast across pspocket's objects does not.
struct TypeDef {
    enum class Kind : u8 { Class, Si, Vmi };
    std::string_view name;
    Kind kind;
    u32 flags = 0;
    std::vector<std::pair<std::string_view, s32>> bases; // (type, offset_flags) for Vmi; the base for Si
};

const std::vector<VtableDef>& vtable_defs();   // stdcxx_streams.cpp: every class pspocket makes objects of
const std::vector<TypeDef>& type_defs();
const std::vector<VtableDef>& facet_vtable_defs(); // stdcxx_facets.cpp: the facets'
const std::vector<TypeDef>& facet_type_defs();

// The definition of a class's vtable or type_info in either part's list (null when neither has it).
inline const VtableDef* find_vtable_def(std::string_view cls)
{
    for (const auto* defs : {&vtable_defs(), &facet_vtable_defs()})
        for (const VtableDef& d : *defs)
            if (d.cls == cls) return &d;
    return nullptr;
}

inline const TypeDef* find_type_def(std::string_view type)
{
    for (const auto* defs : {&type_defs(), &facet_type_defs()})
        for (const TypeDef& d : *defs)
            if (d.name == type) return &d;
    return nullptr;
}

// Guest memory for the library's own data (vtables, type_info, constant strings, its static objects): 64 KB blocks of
// the main area, handed out under the model's lock.
struct Model {
    std::recursive_mutex mutex;
    u32 block = 0, used = 0;
    std::unordered_map<std::string_view, std::vector<u32>> vtables; // class -> its address points
    std::unordered_map<std::string_view, u32> types;
    std::unordered_map<std::string, u32> strings;
    std::unordered_map<std::string, u32> objects; // the library's static objects, by name

    u32 alloc(u32 size, u32 align = 8)
    {
        used = (used + align - 1) & ~(align - 1);
        if (!block || used + size > 0x10000) {
            if (size > 0x10000) PS_FATAL(kChan, "a static object of {:#x} bytes", size);
            block = HleProcess::current()->main_memory().alloc(0x10000, 0x10000);
            if (!block) PS_FATAL(kChan, "no main-area memory for the C++ library's data");
            used = 0;
        }
        const u32 a = block + used;
        used += size;
        return a;
    }
};

inline Model& model() { return HleProcess::current()->state<Model>(); }

// A constant NUL-terminated string in the library's data (the same text, the same address).
inline u32 cstring(PPUContext& ctx, std::string_view s)
{
    Model& m = model();
    std::lock_guard lock(m.mutex);
    if (const auto it = m.strings.find(std::string(s)); it != m.strings.end()) return it->second;
    const u32 a = m.alloc(static_cast<u32>(s.size()) + 1, 4);
    u8* b = guest_base(ctx);
    std::memcpy(b + a, s.data(), s.size());
    b[a + s.size()] = 0;
    m.strings.emplace(std::string(s), a);
    return a;
}

// A static object of the library (zeroed the first time; `fresh` tells the caller it was just made).
inline u32 static_object(std::string_view name, u32 size, bool* fresh = nullptr)
{
    Model& m = model();
    std::lock_guard lock(m.mutex);
    if (const auto it = m.objects.find(std::string(name)); it != m.objects.end()) {
        if (fresh) *fresh = false;
        return it->second;
    }
    const u32 a = m.alloc(size, 8);
    std::memset(HleProcess::current()->kernel().memory().base() + a, 0, size);
    m.objects.emplace(std::string(name), a);
    if (fresh) *fresh = true;
    return a;
}

// The descriptor of one of this library's functions (exported or internal), by its mangled name.
inline u32 descriptor(std::string_view name)
{
    const u32 d = HleProcess::current()->link_function(kModule, recomp::nid_of(name), name);
    if (!d) PS_FATAL(kChan, "no descriptor for {}", name);
    return d;
}

inline u32 type_info_locked(PPUContext& ctx, Model& m, std::string_view type)
{
    if (const auto it = m.types.find(type); it != m.types.end()) return it->second;
    const TypeDef* def = find_type_def(type);
    if (!def) PS_FATAL(kChan, "no type_info for {}", type);
    const u32 size = def->kind == TypeDef::Kind::Class ? 8
        : def->kind == TypeDef::Kind::Si               ? 12
                                                       : 16 + 8 * static_cast<u32>(def->bases.size());
    const u32 a = m.alloc(size, 8);
    m.types.emplace(type, a);
    u8* b = guest_base(ctx);
    ps_write32(b, a, 0);
    ps_write32(b, a + 4, cstring(ctx, type));
    if (def->kind == TypeDef::Kind::Si) {
        ps_write32(b, a + 8, type_info_locked(ctx, m, def->bases.at(0).first));
    } else if (def->kind == TypeDef::Kind::Vmi) {
        ps_write32(b, a + 8, def->flags);
        ps_write32(b, a + 12, static_cast<u32>(def->bases.size()));
        for (usize i = 0; i < def->bases.size(); i++) {
            ps_write32(b, a + 16 + 8 * static_cast<u32>(i), type_info_locked(ctx, m, def->bases[i].first));
            ps_write32(b, a + 20 + 8 * static_cast<u32>(i), static_cast<u32>(def->bases[i].second));
        }
    }
    return a;
}

// The type_info of a class of the library, by its mangled type name.
inline u32 type_info(PPUContext& ctx, std::string_view type)
{
    Model& m = model();
    std::lock_guard lock(m.mutex);
    return type_info_locked(ctx, m, type);
}

// The address point `point` (0: the primary vtable) of the vtable of class `cls`, laid out the first time.
inline u32 vtable(PPUContext& ctx, std::string_view cls, u32 point = 0)
{
    Model& m = model();
    std::lock_guard lock(m.mutex);
    if (const auto it = m.vtables.find(cls); it != m.vtables.end()) return it->second.at(point);
    const VtableDef* def = find_vtable_def(cls);
    if (!def) PS_FATAL(kChan, "no vtable for {}", cls);
    const u32 a = m.alloc(4 * static_cast<u32>(def->words.size()), 8);
    u8* b = guest_base(ctx);
    for (usize i = 0; i < def->words.size(); i++) {
        const VWord& w = def->words[i];
        u32 v = 0;
        switch (w.kind) {
        case VWord::Kind::Value: v = static_cast<u32>(w.value); break;
        case VWord::Kind::Type: v = type_info_locked(ctx, m, w.name); break;
        case VWord::Kind::Func: v = descriptor(w.name); break;
        }
        ps_write32(b, a + 4 * static_cast<u32>(i), v);
    }
    std::vector<u32> points;
    for (const u32 p : def->points) points.push_back(a + 4 * p);
    const u32 r = points.at(point);
    m.vtables.emplace(cls, std::move(points));
    return r;
}

// ------------------------------------------------------------------------------------------------- strings

// basic_string<char> (E = 1) and basic_string<wchar_t> (E = 4), Dinkumware's of VC8 (xstring): 28 bytes, the
// allocator's byte at +0, then _Bx (16 bytes: the small buffer, or the pointer to the heap's) at +4, _Mysize at +20,
// _Myres at +24. A string of up to _BUF_SIZE - 1 elements lives in _Bx; capacities grow as _ALLOC_MASK and half again
// (_Copy), the heap's block from the library's operator new.
template <u32 E>
struct StrLayout {
    static constexpr u32 kBuf = 16 / E;                        // _BUF_SIZE
    static constexpr u32 kMask = E == 1 ? 15 : 3;              // _ALLOC_MASK
    static constexpr u32 kMax = (0xFFFFFFFFu / E) - 1;         // max_size(): allocator's max_size() - 1
    static constexpr u32 kBx = 4, kMysize = 20, kMyres = 24, kBytes = 28;
};

// Raises length_error("string too long") and out_of_range("invalid string position"): _String_base::_Xlen and _Xran
// (stdcxx_core.hpp).
[[noreturn]] void xlen(PPUContext& ctx);
[[noreturn]] void xran(PPUContext& ctx);

// A guest string object and the operations the library's out-of-line members do on it (VC8's algorithms, so the
// capacities the game's inline code sees are the SDK's).
template <u32 E>
class GStr {
public:
    using L = StrLayout<E>;

    GStr(G& g, u32 self) : g_(g), self_(self) {}

    u32 self() const { return self_; }
    u32 size() const { return g_.r32(self_ + L::kMysize); }
    u32 res() const { return g_.r32(self_ + L::kMyres); }
    // _Myptr()
    u32 ptr() const { return res() >= L::kBuf ? g_.r32(self_ + L::kBx) : self_ + L::kBx; }
    u32 at(u32 i) const { return g_.template rel<E>(ptr() + E * i); }

    // _Eos: the size and the terminating NUL.
    void eos(u32 n) const
    {
        g_.w32(self_ + L::kMysize, n);
        g_.template wel<E>(ptr() + E * n, 0);
    }

    // _Tidy(_Built, _Newsize)
    void tidy(bool built, u32 newsize) const
    {
        if (built && L::kBuf <= res()) {
            const u32 p = g_.r32(self_ + L::kBx);
            if (newsize > 0) move(self_ + L::kBx, p, newsize);
            op_delete(g_.ctx, p);
        }
        g_.w32(self_ + L::kMyres, L::kBuf - 1);
        eos(newsize);
    }

    // _Copy(_Newsize, _Oldlen): a new block for at least _Newsize elements, the first _Oldlen copied over.
    void copy(u32 newsize, u32 oldlen) const
    {
        const u32 myres = res();
        u32 newres = newsize | L::kMask;
        if (L::kMax < newres) newres = newsize;
        else if (newres / 3 < myres / 2 && myres <= L::kMax - myres / 2) newres = myres + myres / 2;
        const u64 bytes = (u64{newres} + 1) * E;
        const u32 p = op_new(g_.ctx, bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<u32>(bytes));
        if (oldlen > 0) move(p, ptr(), oldlen);
        tidy(true, 0);
        g_.w32(self_ + L::kBx, p);
        g_.w32(self_ + L::kMyres, newres);
        eos(oldlen);
    }

    // _Grow(_Newsize, _Trim): true when there is something to do.
    bool grow(u32 newsize, bool trim) const
    {
        if (L::kMax < newsize) xlen(g_.ctx);
        if (res() < newsize) copy(newsize, size());
        else if (trim && newsize < L::kBuf) tidy(true, newsize < size() ? newsize : size());
        else if (newsize == 0) eos(0);
        return 0 < newsize;
    }

    // Elements in guest memory, overlapping or not (_Traits::move).
    void move(u32 dst, u32 src, u32 count) const
    {
        if (!count || dst == src) return;
        std::memmove(g_.b + dst, g_.b + src, u64{count} * E);
    }
    void fill(u32 dst, u32 count, u32 ch) const
    {
        if constexpr (E == 1) {
            std::memset(g_.b + dst, static_cast<int>(ch & 0xFF), count);
        } else {
            for (u32 i = 0; i < count; i++) g_.w32(dst + 4 * i, ch);
        }
    }

    // Whether `p` points into the string's elements (_Inside).
    bool inside(u32 p) const { return p != 0 && ptr() <= p && p < ptr() + E * size(); }

    // The elements, read out to the host.
    std::vector<u8> bytes(u32 off, u32 count) const
    {
        std::vector<u8> v(u64{count} * E);
        if (count) std::memcpy(v.data(), g_.b + ptr() + E * off, v.size());
        return v;
    }
    std::string text() const
    {
        std::string s;
        for (u32 i = 0; i < size(); i++) s += static_cast<char>(at(i));
        return s;
    }

    // basic_string(): an empty string.
    void construct() const { tidy(false, 0); }

    // assign(const _Elem *, size_type) for elements outside the string (the host copy `src`).
    void assign_bytes(const std::vector<u8>& src, u32 num) const
    {
        if (grow(num, false)) {
            if (num) std::memcpy(g_.b + ptr(), src.data(), u64{num} * E);
            eos(num);
        }
    }

    // assign(const _Myt& _Right, _Roff, _Count)
    void assign(u32 right, u32 roff, u32 count) const
    {
        const GStr r(g_, right);
        if (r.size() < roff) xran(g_.ctx);
        u32 num = r.size() - roff;
        if (count < num) num = count;
        if (right == self_) {
            erase(roff + num, npos);
            erase(0, roff);
        } else if (grow(num, false)) {
            move(ptr(), r.ptr() + E * roff, num);
            eos(num);
        }
    }

    // erase(_Off, _Count)
    void erase(u32 off, u32 count) const
    {
        const u32 mysize = size();
        if (mysize < off) xran(g_.ctx);
        if (mysize - off < count) count = mysize - off;
        if (0 < count) {
            move(ptr() + E * off, ptr() + E * (off + count), mysize - off - count);
            eos(mysize - count);
        }
    }

    // append(_Count, _Ch)
    void append(u32 count, u32 ch) const
    {
        const u32 mysize = size();
        if (npos - mysize <= count) xlen(g_.ctx);
        u32 num = 0;
        if (0 < count && grow(num = mysize + count, false)) {
            fill(ptr() + E * mysize, count, ch);
            eos(num);
        }
    }

    // insert(_Off, _Count, _Ch)
    void insert(u32 off, u32 count, u32 ch) const
    {
        const u32 mysize = size();
        if (mysize < off) xran(g_.ctx);
        if (npos - mysize <= count) xlen(g_.ctx);
        u32 num = 0;
        if (0 < count && grow(num = mysize + count, false)) {
            move(ptr() + E * (off + count), ptr() + E * off, mysize - off);
            fill(ptr() + E * off, count, ch);
            eos(num);
        }
    }

    // replace(_Off, _N0, const _Elem *_Ptr, _Count). Elements from inside the string are read out first, which gives
    // what VC8's careful substring path gives.
    void replace(u32 off, u32 n0, u32 src, u32 count) const
    {
        if (inside(src)) {
            const u32 roff = (src - ptr()) / E;
            const u32 avail = size() - roff;
            replace_with(off, n0, bytes(roff, count < avail ? count : avail), count < avail ? count : avail, true);
            return;
        }
        std::vector<u8> v(u64{count} * E);
        if (count) std::memcpy(v.data(), g_.b + src, v.size());
        replace_with(off, n0, v, count, false);
    }

    void replace_with(u32 off, u32 n0, const std::vector<u8>& src, u32 count, bool from_self) const
    {
        const u32 mysize = size();
        if (mysize < off) xran(g_.ctx);
        if (mysize - off < n0) n0 = mysize - off;
        if (npos - count <= mysize - n0) xlen(g_.ctx);
        const u32 nm = mysize - n0 - off;
        if (from_self) {
            const u32 newsize = mysize + count - n0;
            if (mysize < newsize) grow(newsize, false);
            move(ptr() + E * (off + count), ptr() + E * (off + n0), nm);
            if (count) std::memcpy(g_.b + ptr() + E * off, src.data(), u64{count} * E);
            eos(newsize);
            return;
        }
        if (count < n0) move(ptr() + E * (off + count), ptr() + E * (off + n0), nm);
        u32 num = 0;
        if ((0 < count || 0 < n0) && grow(num = mysize + count - n0, false)) {
            if (n0 < count) move(ptr() + E * (off + count), ptr() + E * (off + n0), nm);
            if (count) std::memcpy(g_.b + ptr() + E * off, src.data(), u64{count} * E);
            eos(num);
        }
    }

    // A host string's elements assigned (the library's own strings: names, messages).
    void set(std::string_view s) const
    {
        std::vector<u8> v(s.size() * E);
        for (usize i = 0; i < s.size(); i++) {
            if constexpr (E == 1) v[i] = static_cast<u8>(s[i]);
            else ps_st32(v.data() + 4 * i, static_cast<u8>(s[i]));
        }
        assign_bytes(v, static_cast<u32>(s.size()));
    }

    // ~basic_string()
    void destroy() const { tidy(true, 0); }

private:
    G& g_;
    u32 self_;
};

// ------------------------------------------------------------------------------------------------- exceptions

// Raises the exception object at `exc` as exception::_Raise does in the SDK's no-exception library: the raise handler
// (when one is set), the object's _Doraise (its vtable's slot 3), then _Throw, which prints "exception: <what()>" on
// stderr and aborts (sys_libc's abort ends the process). It does not return.
[[noreturn]] void raise_object(PPUContext& ctx, u32 exc);

// A temporary exception of one of the library's classes (by its mangled type name, a string message for logic_error
// and runtime_error kinds) raised from the guest stack.
[[noreturn]] void raise_new(PPUContext& ctx, std::string_view cls, std::string_view message = {});

// ------------------------------------------------------------------------------------------------- ios_base

// ios_base (40 bytes): vptr, size_t _Stdstr (+4), iostate _Mystate (+8), _Except (+12), fmtflags _Fmtfl (+16),
// streamsize _Prec (+20), _Wide (+24), _Iosarray *_Arr (+28), _Fnarray *_Calls (+32), locale *_Ploc (+36); basic_ios
// adds the stream buffer (+40), the tie (+44) and the fill (+48). Its states and format flags are Dinkumware's _Iosb
// values (_IOSskipws ... _IOSboolalpha).
inline constexpr u32 kIosStdstr = 4, kIosState = 8, kIosExcept = 12, kIosFmtfl = 16, kIosPrec = 20, kIosWide = 24,
                     kIosArr = 28, kIosCalls = 32, kIosPloc = 36, kIosBaseBytes = 40;
inline constexpr u32 kIosSb = 40, kIosTie = 44, kIosFill = 48, kBasicIosBytes = 52;
inline constexpr u32 kGoodbit = 0, kEofbit = 1, kFailbit = 2, kBadbit = 4, kStatmask = 0x17;
inline constexpr u32 kSkipws = 0x1, kUnitbuf = 0x2, kUppercase = 0x4, kShowbase = 0x8, kShowpoint = 0x10,
                     kShowpos = 0x20, kLeft = 0x40, kRight = 0x80, kInternal = 0x100, kDec = 0x200, kOct = 0x400,
                     kHex = 0x800, kScientific = 0x1000, kFixed = 0x2000, kBoolalpha = 0x4000, kAdjustfield = 0x1C0,
                     kBasefield = 0xE00, kFloatfield = 0x3000, kFmtmask = 0xFFFF;
inline constexpr s32 kEof = -1;

// ------------------------------------------------------------------------------------------------- between the parts

// What the facets part (stdcxx_facets.cpp) calls in the streams part (stdcxx_streams.cpp) directly, both being this
// library's own code (the facets part's vtables and type_info reach the model through find_vtable_def above): use_facet
// <F>(loc) by F's mangled type name (bad_cast when no facet can be made), and a stream buffer's sgetc, sbumpc and sputc
// as the header's inline members do them.
u32 use_facet_of(PPUContext& ctx, u32 loc, std::string_view type);
s32 streambuf_sgetc(PPUContext& ctx, u32 sb, u32 elem_size);
s32 streambuf_sbumpc(PPUContext& ctx, u32 sb, u32 elem_size);
s32 streambuf_sputc(PPUContext& ctx, u32 sb, u32 elem_size, u32 ch);

} // namespace pspocket::hle::stdcxx
