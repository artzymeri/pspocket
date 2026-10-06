// sysPrxForUser's module functions (D-159): a game loads, starts, stops and unloads PRX modules of its own (and
// firmware ones by path). References: RPCS3 rpcs3/Emu/Cell/Modules/sys_prx_.cpp (the library's wrappers: the "_lv2prx"
// lwmutex, the start and stop entries called on the caller's thread) and lv2/sys_prx.cpp (the syscalls behind them).
// The loader does the placing and linking (HleProcess::module_loader, runtime/loader RuntimeModules); this file runs
// the module's start and stop functions on the calling thread and keeps the order of the calls.

#include <mutex>
#include <string>

#include "guest.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sys_prx";

constexpr u32 kPrxErrorUnknownModule = 0x8001112E;
constexpr u32 kPrxNoLoader = 0x80011881; // CELL_PRX_ERROR_NO_LIBLV2: nothing in this process loads modules
constexpr u64 kLoadFlagFixedAddress = 1;

// RPCS3 takes the library's "_lv2prx" lwmutex around each call; module_start may load other modules on the same
// thread, so a recursive host mutex.
struct PrxCalls {
    std::recursive_mutex mutex;
};

ModuleLoader* loader_of(PPUContext&) { return HleProcess::current() ? HleProcess::current()->module_loader() : nullptr; }

std::string guest_string(lv2::PpuThread& t, u32 addr, u32 max)
{
    std::string s;
    for (u32 i = 0; i < max && guest_readable(t, addr + i, 1); i++) {
        const char c = static_cast<char>(ps_read8(t.kernel.base(), addr + i));
        if (!c) break;
        s += c;
    }
    return s;
}

s32 code(u32 c) { return static_cast<s32>(c); }

s32 load(PPUContext& ctx, u32 path, u64 flags)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!path || !guest_readable(t, path, 1)) return code(lv2::CELL_EFAULT);
    const std::string p = guest_string(t, path, 1024);
    // RPCS3 prx_load_module: flags other than FIXEDADDR are invalid; FIXEDADDR needs a fixed-address process.
    if (flags & ~kLoadFlagFixedAddress) return code(lv2::CELL_EINVAL);
    if (flags) {
        PS_LOG_ERROR(kChan, "sys_prx_load_module({}): a fixed address is not supported: CELL_ENOSYS", p);
        return code(lv2::CELL_ENOSYS);
    }
    ModuleLoader* l = loader_of(ctx);
    if (!l) {
        PS_LOG_ERROR(kChan, "sys_prx_load_module({}): no module loader in this process", p);
        return code(kPrxNoLoader);
    }
    u32 id = 0;
    const u32 r = l->load(p, &id);
    if (r != lv2::CELL_OK) {
        PS_LOG_ERROR(kChan, "sys_prx_load_module({}): {:#010x}", p, r);
        return code(r);
    }
    PS_LOG_INFO(kChan, "sys_prx_load_module({}): id {:#x}", p, id);
    return code(id);
}

// sys_prx_load_module(path, flags, pOpt): the module's ID, or an error.
s32 sys_prx_load_module(PPUContext& ctx, u32 path, u64 flags, u32 /*opt*/)
{
    std::lock_guard lock(HleProcess::current()->state<PrxCalls>().mutex);
    return load(ctx, path, flags);
}

// sys_prx_load_module_list(count, path_list, flags, pOpt, id_list): each path (a 32-bit pointer) loaded in turn.
s32 sys_prx_load_module_list(PPUContext& ctx, s32 count, u32 path_list, u64 flags, u32 /*opt*/, u32 id_list)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (count < 0 || !guest_readable(t, path_list, 4u * static_cast<u32>(count)) ||
        !guest_readable(t, id_list, 4u * static_cast<u32>(count)))
        return code(lv2::CELL_EFAULT);
    std::lock_guard lock(HleProcess::current()->state<PrxCalls>().mutex);
    for (s32 i = 0; i < count; i++) {
        const s32 id = load(ctx, ps_read32(t.kernel.base(), path_list + 4u * static_cast<u32>(i)), flags);
        if (id < 0) return id;
        ps_write32(t.kernel.base(), id_list + 4u * static_cast<u32>(i), static_cast<u32>(id));
    }
    return lv2::CELL_OK;
}

// sys_prx_start_module(id, args, argp, result, flags, pOpt): RPCS3's wrapper: the syscall links the module (cmd 1),
// module_start(args, argp) runs here, its result goes to *result and back to the syscall (cmd 2).
s32 sys_prx_start_module(PPUContext& ctx, u32 id, u32 args, u32 argp, u32 result, u64 /*flags*/, u32 /*opt*/)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!result || !guest_readable(t, result, 4)) return code(lv2::CELL_EINVAL);
    ModuleLoader* l = loader_of(ctx);
    if (!l) return code(kPrxNoLoader);
    std::lock_guard lock(HleProcess::current()->state<PrxCalls>().mutex);
    u32 entry = 0;
    if (const u32 r = l->start(id, &entry); r != lv2::CELL_OK) return code(r);
    // Firmware modules loaded for it start first, as at boot (D-175).
    for (const u32 dep : l->take_dependency_starts()) {
        const u32 r = static_cast<u32>(call_guest(ctx, dep, {0, 0}));
        PS_LOG_INFO(kChan, "sys_prx_start_module({:#x}): firmware module_start {:#x} returned {:#x}", id, dep, r);
    }
    const s32 res = entry ? static_cast<s32>(call_guest(ctx, entry, {args, argp})) : 0;
    ps_write32(t.kernel.base(), result, static_cast<u32>(res));
    l->started(id, res);
    PS_LOG_INFO(kChan, "sys_prx_start_module({:#x}): module_start {:#x} returned {:#x}", id, entry, static_cast<u32>(res));
    return lv2::CELL_OK;
}

// sys_prx_stop_module(id, args, argp, result, flags, pOpt): as starting, with module_stop.
s32 sys_prx_stop_module(PPUContext& ctx, u32 id, u32 args, u32 argp, u32 result, u64 /*flags*/, u32 /*opt*/)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!result || !guest_readable(t, result, 4)) return code(lv2::CELL_EINVAL);
    ModuleLoader* l = loader_of(ctx);
    if (!l) return code(kPrxNoLoader);
    std::lock_guard lock(HleProcess::current()->state<PrxCalls>().mutex);
    u32 entry = 0;
    if (const u32 r = l->stop(id, &entry); r != lv2::CELL_OK) return code(r);
    const s32 res = entry ? static_cast<s32>(call_guest(ctx, entry, {args, argp})) : 0;
    ps_write32(t.kernel.base(), result, static_cast<u32>(res));
    l->stopped(id, res);
    return lv2::CELL_OK;
}

s32 sys_prx_unload_module(PPUContext& ctx, u32 id, u64 /*flags*/, u32 /*opt*/)
{
    ModuleLoader* l = loader_of(ctx);
    if (!l) return code(kPrxNoLoader);
    std::lock_guard lock(HleProcess::current()->state<PrxCalls>().mutex);
    return code(l->unload(id));
}

// sys_prx_get_module_id_by_name(name, flags, pOpt): by the module info's name (up to 28 characters).
s32 sys_prx_get_module_id_by_name(PPUContext& ctx, u32 name, u64 /*flags*/, u32 /*opt*/)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!name || !guest_readable(t, name, 1)) return code(lv2::CELL_EINVAL);
    ModuleLoader* l = loader_of(ctx);
    const std::string n = guest_string(t, name, 28);
    const u32 id = l ? l->id_by_name(n) : 0;
    if (!id) {
        PS_LOG_WARN(kChan, "sys_prx_get_module_id_by_name({}): unknown", n);
        return code(kPrxErrorUnknownModule);
    }
    return code(id);
}

} // namespace

void register_sys_prx(Registry& r)
{
    r.add("sysPrxForUser", {
        PS_HLE_FUNC(0x26090058, sys_prx_load_module),
        PS_HLE_FUNC(0xB27C8AE7, sys_prx_load_module_list),
        PS_HLE_FUNC(0x9F18429D, sys_prx_start_module),
        PS_HLE_FUNC(0x80FB0C19, sys_prx_stop_module),
        PS_HLE_FUNC(0xF0AECE0D, sys_prx_unload_module),
        PS_HLE_FUNC(0xE0998DBF, sys_prx_get_module_id_by_name),
    });
}

} // namespace pspocket::hle
