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

#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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
};

class Registry {
public:
    // Adds a module's functions (their `module` field is set to `module`). A duplicate (module, NID) aborts.
    void add(std::string_view module, std::initializer_list<Function> list);
    std::span<const Function> functions() const { return functions_; }
    const Function* find(std::string_view module, u32 nid) const;

private:
    std::vector<Function> functions_;
};

// Every module in src/modules.cpp, built on first use.
const Registry& registry();

// ---- typed binding: R fn(PPUContext&, A...) with A in {u32, s32, u64, s64}, R in {void, s32, u32, u64} ----

namespace detail {

template <class T>
T reg_arg(u64 v)
{
    if constexpr (std::is_same_v<T, u64>) return v;
    else if constexpr (std::is_same_v<T, s64>) return static_cast<s64>(v);
    else if constexpr (std::is_same_v<T, u32>) return static_cast<u32>(v); // low word, as RPCS3 ppu_gpr_cast
    else if constexpr (std::is_same_v<T, s32>) return static_cast<s32>(static_cast<u32>(v));
    else static_assert(sizeof(T) == 0, "unsupported HLE argument type");
}

template <class R, class... A, usize... I>
void invoke(R (*fn)(PPUContext&, A...), PPUContext& ctx, std::index_sequence<I...>)
{
    static_assert(sizeof...(A) <= 8, "HLE functions take at most 8 register arguments (r3..r10)");
    if constexpr (std::is_void_v<R>) {
        fn(ctx, reg_arg<A>(ctx.gpr[3 + I])...);
    } else if constexpr (std::is_same_v<R, s32>) {
        // error_code results are sign-extended into r3 (RPCS3 ppu_func_detail::bind_result).
        ctx.gpr[3] = static_cast<u64>(static_cast<s64>(fn(ctx, reg_arg<A>(ctx.gpr[3 + I])...)));
    } else if constexpr (std::is_same_v<R, u32> || std::is_same_v<R, u64>) {
        ctx.gpr[3] = fn(ctx, reg_arg<A>(ctx.gpr[3 + I])...);
    } else {
        static_assert(sizeof(R) == 0, "unsupported HLE return type");
    }
}

template <auto Fn>
void thunk(PPUContext& ctx, u8*)
{
    [&]<class R, class... A>(R (*fn)(PPUContext&, A...)) {
        invoke(fn, ctx, std::index_sequence_for<A...>{});
    }(Fn);
}

} // namespace detail

// Registry::add(module, {PS_HLE_FUNC(0x744680A2, sys_initialize_tls), ...})
#define PS_HLE_FUNC(nid, fn) ::pspocket::hle::Function{{}, #fn, nid, &::pspocket::hle::detail::thunk<&fn>}

} // namespace pspocket::hle
