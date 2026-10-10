#pragma once

// Firmware modules implemented on the host (HLE), resolved by module name and NID (plan section 8).
//
// A game's imports are linked at load time (Linkage): every imported function gets a code stub and a function
// descriptor in guest memory, and the stub's address is registered in the PPU dispatch table, so lifted code and the
// interpreter reach the host implementation through the same indirect-call path as any other guest function. A
// function with no host implementation logs `UNIMPLEMENTED module::function (NID)` and returns CELL_ENOSYS
// (CLAUDE.md rule 4), or aborts when the process is configured to stop at the first gap.
//
// Each module is one .cpp with a register_<module>(Registry&) function listed in src/modules.cpp (explicit, not static
// initialisers, which a static-library link would drop). Functions are added only when the inventory or a runtime
// log shows the game uses them. NIDs are written out next to the names; the test suite
// checks each one against the NID hash of its name (recomp/common nid_of), so a typo can't link the wrong function.
//
// References: RPCS3 rpcs3/Emu/Cell/PPUModule.cpp (ppu_load_imports, ppu_register_function_at), PPUFunction.h
// (REG_FUNC, ppu_function_manager), Emu/Cell/Modules/*.cpp (the module implementations each file here cites).

#include <array>
#include <bit>
#include <initializer_list>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/core/types.hpp"
#include "pspocket/ppu/context.h"

namespace pspocket::hle {

// Every HLE function: arguments in r3..r10, result in r3, returns like `blr` (the caller continues at LR).
using HleFn = void (*)(PPUContext& ctx, u8* base);

struct Function {
    std::string_view module;
    std::string_view name;
    u32 nid = 0;
    HleFn fn = nullptr;
    // A function the module hands to the game by address rather than exporting it (RPCS3 FIND_FUNC, e.g.
    // cellGcmCallback in the GCM context). The loader links every internal function of each module the game imports,
    // before guest threads run; HLE code finds its descriptor with HleProcess::descriptor_of.
    bool internal = false;
    // A function made from RPCS3 without Sony's module to compare it with, in a library whose other functions were
    // compared (D-594; PS_HLE_UNVERIFIED). The loader treats an import of it as one of a function the host lacks:
    // where the installed system software has the library's module, that module serves the whole library, as it did
    // before the function was written here; with none, the host's serves it, this function included.
    bool unverified = false;
};

// What a host module takes from the firmware's modules, as the firmware's own module of that library imports it
// (D-197): a codec core's operation table (a variable) or function that pspocket's cellAdec calls for a codec it has no
// core of its own for, as RPCS3's cellAdec calls the LLE cores. The loader chooses the modules for a game importing
// `module` as it chooses the game's own (loader::LleModules) and links each export into HleProcess::link_import; HLE
// code reads it with HleProcess::imported.
struct HostImport {
    std::string_view module;  // the host module importing
    std::string_view library; // the firmware library exporting
    u32 nid = 0;
    bool variable = false;
};

// A variable a host module exports (D-503; RPCS3 REG_VAR): `size` bytes of guest memory aligned to `align`, zeroed and
// then laid out by `init` (when there is one) as the first import of it is linked; every import of it gets that address.
struct Variable {
    std::string_view module;
    std::string_view name;
    u32 nid = 0;
    u32 size = 4;
    u32 align = 4;
    void (*init)(u8* base, u32 addr) = nullptr;
};

class Registry {
public:
    // Adds a module's functions (their `module` field is set to `module`). A duplicate (module, NID) aborts.
    void add(std::string_view module, std::initializer_list<Function> list);
    std::span<const Function> functions() const { return functions_; }
    const Function* find(std::string_view module, u32 nid) const;
    // Adds a module's variables, likewise.
    void add_variables(std::string_view module, std::initializer_list<Variable> list);
    std::span<const Variable> variables() const { return variables_; }
    const Variable* find_variable(std::string_view module, u32 nid) const;
    // Adds what `module` imports from firmware modules (their `module` field is set to `module`).
    void add_imports(std::string_view module, std::span<const HostImport> list);
    std::span<const HostImport> imports() const { return imports_; }
    // Marks a library whose host implementation was made without Sony's module to compare it with (D-563): the
    // loader gives a game the firmware's module for it when the installed system software has one (as it would for a
    // library the host lacks), and the host's when there is none.
    void firmware_first(std::string_view library) { firmware_first_.emplace(library); }
    bool is_firmware_first(std::string_view library) const { return firmware_first_.contains(library); }
    // The same for one function of a library (Function::unverified, D-594): whether the host has it, made without
    // Sony's module to compare it with.
    bool is_unverified(std::string_view module, u32 nid) const
    {
        const Function* f = find(module, nid);
        return f && f->unverified;
    }

private:
    std::vector<Function> functions_;
    std::vector<Variable> variables_;
    std::vector<HostImport> imports_;
    std::set<std::string, std::less<>> firmware_first_;
    std::unordered_map<std::string, usize> index_; // "module:NID" -> functions_ (or variables_ with a 'v' prefix)
};

// Every module in src/modules.cpp, built on first use.
const Registry& registry();

// A function RPCS3's own module leaves as a stub: it logs and returns a constant (D-182). Only for the libraries
// pspocket always serves itself (no firmware fallback) and RPCS3 serves with HLE by default; generated from RPCS3's
// source into src/rpcs3_stubs.inc by scripts/gen_rpcs3_stubs.py. The linkage answers an import with one when the
// registry has no implementation: a log of the call, then `r3` (left alone when !sets_r3, a void function).
struct StubFunction {
    std::string_view module;
    std::string_view name;
    u32 nid = 0;
    u64 r3 = 0;
    bool sets_r3 = true;
};
std::span<const StubFunction> rpcs3_stubs();
const StubFunction* find_rpcs3_stub(std::string_view module, u32 nid);

// ---- typed binding: R fn(PPUContext&, A...) with A in {bool, u8, u16, u32, s32, u64, s64, float, double}, R in {void,
// s32, u32, u64, float, double}; up to 20 arguments (the ninth and later on the caller's stack; sceNpScore's clan
// rankings take 19) ----

namespace detail {

template <class T>
T reg_arg(u64 v)
{
    if constexpr (std::is_same_v<T, u64>) return v;
    else if constexpr (std::is_same_v<T, s64>) return static_cast<s64>(v);
    else if constexpr (std::is_same_v<T, u32>) return static_cast<u32>(v); // low word, as RPCS3 ppu_gpr_cast
    else if constexpr (std::is_same_v<T, s32>) return static_cast<s32>(static_cast<u32>(v));
    else if constexpr (std::is_same_v<T, u16>) return static_cast<u16>(v);
    else if constexpr (std::is_same_v<T, s16>) return static_cast<s16>(static_cast<u16>(v));
    else if constexpr (std::is_same_v<T, u8>) return static_cast<u8>(v);
    else if constexpr (std::is_same_v<T, bool>) return (v & 0xFF) != 0; // b8: the low byte
    else static_assert(sizeof(T) == 0, "unsupported HLE argument type");
}

// Argument I: r3..r10 for the first eight, then the caller's parameter save area at r1 + 0x70 + 8 * (I - 8) (the
// PPC64 ELF ABI; RPCS3 ppu_func_detail reads the same slots). A floating point argument (float, double) comes from
// f1..f13 by its count F among the floating ones, and takes its slot in the sequence all the same (so the integers
// after it move on), past f13 from that slot of the save area (a double, or a float in the slot's low word).
template <class T, usize I, usize F>
T arg(PPUContext& ctx, u8* base)
{
    if constexpr (std::is_floating_point_v<T>) {
        if constexpr (F < 13) {
            return static_cast<T>(ctx.fpr[1 + F]);
        } else {
            const u32 slot = static_cast<u32>(ctx.gpr[1]) + 0x30 + 8 * static_cast<u32>(I);
            if constexpr (std::is_same_v<T, float>) return std::bit_cast<float>(ps_read32(base, slot + 4));
            else return std::bit_cast<double>(ps_read64(base, slot));
        }
    } else if constexpr (I < 8) {
        return reg_arg<T>(ctx.gpr[3 + I]);
    } else {
        return reg_arg<T>(ps_read64(base, static_cast<u32>(ctx.gpr[1]) + 0x70 + 8 * static_cast<u32>(I - 8)));
    }
}

// For each argument, its count among the floating point ones before it.
template <class... A>
constexpr std::array<usize, sizeof...(A) + 1> fp_ordinals()
{
    std::array<usize, sizeof...(A) + 1> out{};
    usize n = 0, i = 0;
    ((out[i++] = std::is_floating_point_v<A> ? n++ : n), ...);
    return out;
}

template <class R, class... A, usize... I>
void invoke(R (*fn)(PPUContext&, A...), PPUContext& ctx, u8* base, std::index_sequence<I...>)
{
    static_assert(sizeof...(A) <= 20, "HLE functions take at most 20 arguments");
    constexpr auto F = fp_ordinals<A...>();
    if constexpr (std::is_void_v<R>) {
        fn(ctx, arg<A, I, F[I]>(ctx, base)...);
    } else if constexpr (std::is_same_v<R, s32>) {
        // error_code results are sign-extended into r3 (RPCS3 ppu_func_detail::bind_result).
        ctx.gpr[3] = static_cast<u64>(static_cast<s64>(fn(ctx, arg<A, I, F[I]>(ctx, base)...)));
    } else if constexpr (std::is_same_v<R, u32> || std::is_same_v<R, u64>) {
        ctx.gpr[3] = fn(ctx, arg<A, I, F[I]>(ctx, base)...);
    } else if constexpr (std::is_floating_point_v<R>) {
        ctx.fpr[1] = static_cast<double>(fn(ctx, arg<A, I, F[I]>(ctx, base)...)); // f1, a float in double format
    } else {
        static_assert(sizeof(R) == 0, "unsupported HLE return type");
    }
}

template <auto Fn>
void thunk(PPUContext& ctx, u8* base)
{
    [&]<class R, class... A>(R (*fn)(PPUContext&, A...)) {
        invoke(fn, ctx, base, std::index_sequence_for<A...>{});
    }(Fn);
}

} // namespace detail

// Registry::add(module, {PS_HLE_FUNC(0x744680A2, sys_initialize_tls), ...})
#define PS_HLE_FUNC(nid, fn) ::pspocket::hle::Function{{}, #fn, nid, &::pspocket::hle::detail::thunk<&fn>, false}
// An internal function (Function::internal), with the NID its name hashes to.
#define PS_HLE_INTERNAL(nid, fn) ::pspocket::hle::Function{{}, #fn, nid, &::pspocket::hle::detail::thunk<&fn>, true}
// A function made from RPCS3 without Sony's module to compare it with (Function::unverified, D-594).
#define PS_HLE_UNVERIFIED(nid, fn) \
    ::pspocket::hle::Function{{}, #fn, nid, &::pspocket::hle::detail::thunk<&fn>, false, true}
// Registry::add_variables(module, {PS_HLE_VAR(0x12345678, _Stdin, 4, 4, init_stdin), ...})
#define PS_HLE_VAR(nid, name, size, align, init) ::pspocket::hle::Variable{{}, #name, nid, size, align, init}

} // namespace pspocket::hle
