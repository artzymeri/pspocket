#pragma once

// What pspocket's video codec cores share (D-920): the guest checks Sony's cores make, a call to another HLE library's
// function as a core module calls its import, and the cores' registration (vdec_cores.cpp registers the DivX cores and
// calls the others' own: vdec_smvd4.cpp, ...).

#include <initializer_list>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle::vdec_core {

constexpr s32 error(u32 code) { return static_cast<s32>(code); }

// A guest pointer a core writes through: not null, and readable (pspocket's check where Sony's core would fault).
inline bool writable(PPUContext& ctx, u32 addr, u32 size) { return addr && guest_readable(thread_of(ctx), addr, size); }

// Another HLE library's function called as the core module calls its import (cellSpurs), on this thread.
inline u64 call_import(PPUContext& ctx, std::string_view module, u32 nid, std::initializer_list<u64> args)
{
    const Function* f = registry().find(module, nid);
    if (!f) PS_FATAL("vdecCores", "{}'s function {:#010x} is not pspocket's own", module, nid);
    u64 saved[8];
    for (u32 k = 0; k < 8; k++) saved[k] = ctx.gpr[3 + k];
    u32 i = 0;
    for (const u64 a : args) ctx.gpr[3 + i++] = a;
    f->fn(ctx, guest_base(ctx));
    const u64 r = ctx.gpr[3];
    for (u32 k = 0; k < 8; k++) ctx.gpr[3 + k] = saved[k];
    return r;
}

// The cores registered in files of their own.
void register_smvd4(Registry& r);
void register_smvd2(Registry& r);

} // namespace pspocket::hle::vdec_core
