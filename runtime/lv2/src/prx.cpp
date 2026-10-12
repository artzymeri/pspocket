// lv2's PRX and overlay calls (460-499, 450-458; D-527 part 3). Reference: RPCS3 rpcs3/Emu/Cell/lv2/sys_prx.cpp
// and .h (the option blocks, the start and stop commands, the error codes), sys_overlay.cpp.
//
// pspocket's liblv2 is native: its sys_prx_* functions (hle sys_prx.cpp) work the module loader directly. These are
// the system calls under them, for code that makes them itself (Sony's liblv2coredump and sys_audio, a game's own
// `sc`); they reach the same loader through Kernel::module_host (the HLE layer's), with RPCS3's contract:
// - _sys_prx_start_module(id, flags, opt {size, cmd, entry, res, entry2}): command 1 links the module (and starts the
//   firmware modules loaded for it, D-175) and gives its module_start descriptor in entry and its module_prologue's
//   in entry2 (~0: none; liblv2 calls entry2 with entry when there is one, D-916 part 1); command 2 hands back
//   module_start's result: 0 (resident) keeps the module, another value in its low word unloads it and is returned.
//   _sys_prx_stop_module likewise with module_stop and module_epilogue (command 2: 0 stops it, 1 is
//   CELL_PRX_ERROR_CAN_NOT_STOP); command 4 gives the entries, 8 is taken.
// - The loads by path, by descriptor (the file's own path, `offset` bytes in), on a memory container (there are none
//   for modules: as without), and by list (64-bit pointers to the paths; on a failure the ones loaded are unloaded and
//   the ID list filled with -1).
// - _sys_prx_get_module_list(flags, opt {size 0x20, pad, max, count, idlist}) lists only with flags bit 1;
//   _sys_prx_get_module_info(id, flags, opt {size 0x10, info}) writes the info as liblv2's function does.
// - _sys_prx_register_module: a module's stubs for the system software only (a game: CELL_PRX_ERROR_ELF_IS_REGISTERED,
//   not an error); the library calls RPCS3 leaves as todos answer CELL_OK; register_library checks the address.
// - The debugger's module lists (460, 492, 493) are root's or debug's: CELL_ENOSYS for a game; for root, as the lists
//   above.
// Overlays (an overlay-enabled process: the executable's ppc_seg): a game without it gets CELL_ENOSYS, as RPCS3
// answers; one with it would have its overlay placed at a fixed address in the 0x30000000 segment, which pspocket's
// loader does not do yet: logged, CELL_ENOEXEC. No overlay is ever loaded, so the overlay queries find none.

#include <cstring>
#include <string>
#include <vector>

#include "internal.hpp"

namespace pspocket::lv2 {

namespace {

constexpr std::string_view kPrxChan = "sys_prx";

enum : u32 {
    CELL_PRX_ERROR_ERROR = 0x80011001,
    CELL_PRX_ERROR_UNKNOWN_MODULE = 0x8001112E,
    CELL_PRX_ERROR_CAN_NOT_STOP = 0x80011136,
    CELL_PRX_ERROR_NO_LIBLV2 = 0x80011881,
    CELL_PRX_ERROR_ELF_IS_REGISTERED = 0x80011910,
};
constexpr u64 SYS_PRX_LOAD_MODULE_FLAGS_FIXEDADDR = 1;

Result prx_err(u32 e) { return static_cast<Result>(e); }

ModuleHost* host(PpuThread& t) { return t.kernel.module_host(); }

std::optional<std::string> read_path(const Guest& g, u32 addr)
{
    std::string out;
    if (g.read_cstr(addr, 1024, out) != Guest::Str::Ok) return std::nullopt;
    return out;
}

Result load(PpuThread& t, std::string_view path, u64 offset, u64 flags)
{
    if (flags) {
        if (flags & ~SYS_PRX_LOAD_MODULE_FLAGS_FIXEDADDR) return err(CELL_EINVAL);
        if (!t.kernel.config().ppc_seg) return err(CELL_ENOSYS);
        PS_LOG_ERROR(kPrxChan, "loading {} at a fixed address is not supported (RPCS3 aborts too)", path);
        return err(CELL_ENOSYS);
    }
    ModuleHost* h = host(t);
    if (!h) return prx_err(CELL_PRX_ERROR_NO_LIBLV2);
    u32 id = 0;
    if (const u32 r = h->load(path, offset, &id); r != CELL_OK) {
        PS_LOG_ERROR(kPrxChan, "load {} (offset {:#x}): {:#010x}", path, offset, r);
        return static_cast<Result>(r);
    }
    PS_LOG_INFO(kPrxChan, "loaded {} (offset {:#x}): id {:#x}", path, offset, id);
    return static_cast<Result>(id);
}

Result load_path(PpuThread& t, u32 path, u64 flags)
{
    const auto p = read_path(t.kernel.impl().guest, path);
    if (!p) return err(CELL_EFAULT);
    return load(t, *p, 0, flags);
}

Result load_fd(PpuThread& t, s32 fd, u64 offset, u64 flags)
{
    const auto p = fs_file_path(t.kernel, fd);
    if (!p) return err(CELL_EBADF);
    return load(t, *p, offset, flags);
}

Result load_list(PpuThread& t, s32 count, u32 path_list, u64 flags, u32 id_list)
{
    const Guest& g = t.kernel.impl().guest;
    if (count < 0) return err(CELL_EINVAL);
    if (flags) {
        if (flags & ~SYS_PRX_LOAD_MODULE_FLAGS_FIXEDADDR) return err(CELL_EINVAL);
        return err(CELL_ENOSYS);
    }
    const u32 n = static_cast<u32>(count);
    if (n && (!g.valid(path_list, u64{n} * 8) || !g.valid(id_list, u64{n} * 4))) return err(CELL_EFAULT);
    for (u32 i = 0; i < n; i++) {
        const Result r = load_path(t, static_cast<u32>(g.r64(path_list + 8 * i)), 0);
        if (r < 0) {
            if (ModuleHost* h = host(t)) {
                for (u32 k = 0; k < i; k++) h->unload(g.r32(id_list + 4 * k));
            }
            std::memset(g.ptr(id_list), 0xFF, u64{n} * 4);
            return r;
        }
        g.w32(id_list + 4 * i, static_cast<u32>(r));
    }
    return CELL_OK;
}

bool list_modules(PpuThread& t, u32 idlist, u32 max, u32& count)
{
    const Guest& g = t.kernel.impl().guest;
    count = 0;
    ModuleHost* h = host(t);
    if (!h || !max) return true;
    for (const u32 id : h->list()) {
        if (count >= max) break;
        if (!g.put32(idlist + 4 * count, id)) return false;
        count++;
    }
    return true;
}

Result debugger_only(PpuThread& t, std::string_view name)
{
    if (t.kernel.config().root) return CELL_OK;
    PS_LOG_WARN(kPrxChan, "{}: a debugger's call (root or debug), not a game's: CELL_ENOSYS", name);
    return err(CELL_ENOSYS);
}

} // namespace

// ------------------------------------------------------------------------------------------------- loading

Result _sys_prx_load_module(PpuThread& t, u32 path, u64 flags, u32) { return load_path(t, path, flags); }

Result _sys_prx_load_module_on_memcontainer(PpuThread& t, u32 path, u32, u64 flags, u32)
{
    return load_path(t, path, flags);
}

Result _sys_prx_load_module_by_fd(PpuThread& t, s32 fd, u64 offset, u64 flags, u32) { return load_fd(t, fd, offset, flags); }

Result _sys_prx_load_module_on_memcontainer_by_fd(PpuThread& t, s32 fd, u64 offset, u32, u64 flags, u32)
{
    return load_fd(t, fd, offset, flags);
}

Result _sys_prx_load_module_list(PpuThread& t, s32 count, u32 path_list, u64 flags, u32, u32 id_list)
{
    return load_list(t, count, path_list, flags, id_list);
}

Result _sys_prx_load_module_list_on_memcontainer(PpuThread& t, s32 count, u32 path_list, u32, u64 flags, u32,
    u32 id_list)
{
    return load_list(t, count, path_list, flags, id_list);
}

// ------------------------------------------------------------------------------------------------- start, stop, unload

// _sys_prx_start_module(id, flags, opt): see the top.
Result _sys_prx_start_module(PpuThread& t, u32 id, u64, u32 opt)
{
    const Guest& g = t.kernel.impl().guest;
    if (id == 0 || !opt) return err(CELL_EINVAL);
    if (!g.valid(opt, 0x28)) return err(CELL_EFAULT);
    ModuleHost* h = host(t);
    if (!h) return prx_err(CELL_PRX_ERROR_NO_LIBLV2);
    u32 start = 0, stop = 0;
    if (!h->entries(id, &start, &stop)) return err(CELL_ESRCH);
    const u64 size = g.r64(opt);
    switch (g.r64(opt + 8) & 0xF) {
    case 1: {
        u32 entry = 0, entry2 = 0;
        if (const u32 r = h->start(t, id, &entry, &entry2); r != CELL_OK) return prx_err(CELL_PRX_ERROR_ERROR);
        g.w64(opt + 16, entry ? entry : ~u64{0});
        if (size != 0x20) g.w64(opt + 32, entry2 ? entry2 : ~u64{0});
        return CELL_OK;
    }
    case 2: {
        const u64 res = g.r64(opt + 24);
        h->started(id, static_cast<s32>(res));
        if (res == 0) return CELL_OK;
        if (res & 0xFFFFFFFF) return static_cast<Result>(static_cast<s32>(res)); // unloaded by the loader
        PS_LOG_ERROR(kPrxChan, "_sys_prx_start_module({:#x}): module_start returned {:#x}", id, res);
        return CELL_OK;
    }
    default: return prx_err(CELL_PRX_ERROR_ERROR);
    }
}

// _sys_prx_stop_module(id, flags, opt): see the top.
Result _sys_prx_stop_module(PpuThread& t, u32 id, u64, u32 opt)
{
    const Guest& g = t.kernel.impl().guest;
    ModuleHost* h = host(t);
    if (!h) return prx_err(CELL_PRX_ERROR_NO_LIBLV2);
    u32 start = 0, stop = 0;
    if (!h->entries(id, &start, &stop)) return err(CELL_ESRCH);
    if (!opt) return err(CELL_EINVAL);
    if (!g.valid(opt, 0x28)) return err(CELL_EFAULT);
    const u64 size = g.r64(opt);
    const u64 cmd = g.r64(opt + 8);
    switch (cmd & 0xF) {
    case 1: {
        u32 entry = 0, entry2 = 0;
        if (const u32 r = h->stop(id, &entry, &entry2); r != CELL_OK) return static_cast<Result>(r);
        g.w64(opt + 16, entry ? entry : ~u64{0});
        if (size != 0x20) g.w64(opt + 32, entry2 ? entry2 : ~u64{0});
        return CELL_OK;
    }
    case 2: {
        const u64 res = g.r64(opt + 24);
        if (res == 0) {
            h->stopped(id, 0);
            return CELL_OK;
        }
        if (res == 1) return prx_err(CELL_PRX_ERROR_CAN_NOT_STOP);
        return CELL_OK;
    }
    case 4:
    case 8:
        if (cmd == 4) {
            g.w64(opt + 16, stop ? stop : ~u64{0});
            if (size != 0x20) g.w64(opt + 32, ~u64{0});
        }
        return CELL_OK;
    default: return prx_err(CELL_PRX_ERROR_ERROR);
    }
}

Result _sys_prx_unload_module(PpuThread& t, u32 id, u64, u32)
{
    ModuleHost* h = host(t);
    if (!h) return prx_err(CELL_PRX_ERROR_NO_LIBLV2);
    return static_cast<Result>(h->unload(id));
}

// ------------------------------------------------------------------------------------------------- queries

Result _sys_prx_get_module_list(PpuThread& t, u64 flags, u32 opt)
{
    const Guest& g = t.kernel.impl().guest;
    if (!(flags & 2)) return CELL_OK;
    if (!g.valid(opt, 0x20)) return err(CELL_EFAULT);
    if (g.r64(opt) != 0x20) return CELL_OK; // RPCS3: another structure (0x18) is not served
    u32 count = 0;
    if (!list_modules(t, g.r32(opt + 20), g.r32(opt + 12), count)) return err(CELL_EFAULT);
    g.w32(opt + 16, count);
    return CELL_OK;
}

Result _sys_prx_get_module_info(PpuThread& t, u32 id, u64, u32 opt)
{
    const Guest& g = t.kernel.impl().guest;
    if (!opt) return err(CELL_EFAULT);
    if (!g.valid(opt, 0x10)) return err(CELL_EFAULT);
    if (g.r64(opt) != 0x10) return err(CELL_EINVAL);
    const u32 info = g.r32(opt + 8);
    if (!info) return err(CELL_EFAULT);
    if (!g.valid(info, 8)) return err(CELL_EFAULT);
    const u64 size = g.r64(info);
    if (size != 0x48 && size != 0x58) return err(CELL_EINVAL);
    ModuleHost* h = host(t);
    if (!h) return prx_err(CELL_PRX_ERROR_UNKNOWN_MODULE);
    return h->write_info(t, id, info);
}

Result _sys_prx_get_module_id_by_name(PpuThread& t, u32 name, u64, u32)
{
    std::string n;
    if (t.kernel.impl().guest.read_cstr(name, 28, n) == Guest::Str::Fault) return err(CELL_EINVAL);
    ModuleHost* h = host(t);
    const u32 id = h ? h->id_by_name(n) : 0;
    return id ? static_cast<Result>(id) : prx_err(CELL_PRX_ERROR_UNKNOWN_MODULE);
}

Result _sys_prx_get_module_id_by_address(PpuThread& t, u32 addr)
{
    if (!t.kernel.impl().guest.valid(addr, 1)) return prx_err(CELL_PRX_ERROR_UNKNOWN_MODULE);
    ModuleHost* h = host(t);
    const u32 id = h ? h->id_by_address(addr) : 0;
    return id ? static_cast<Result>(id) : prx_err(CELL_PRX_ERROR_UNKNOWN_MODULE);
}

// ------------------------------------------------------------------------------------------------- registration

Result _sys_prx_register_module(PpuThread& t, u32, u32 opt)
{
    const Guest& g = t.kernel.impl().guest;
    if (!opt) return err(CELL_EINVAL);
    if (!g.valid(opt, 8)) return err(CELL_EFAULT);
    const u64 size = g.r64(opt);
    u64 type = 0;
    switch (size) {
    case 0x1C:
    case 0x20: type = 0; break; // the older layouts carry no type (RPCS3 rebuilds them with type 0)
    case 0x30:
        if (!g.valid(opt, 0x30)) return err(CELL_EFAULT);
        type = g.r64(opt + 8);
        break;
    default: return err(CELL_EINVAL);
    }
    if ((type & 1) && !t.kernel.config().root) return prx_err(CELL_PRX_ERROR_ELF_IS_REGISTERED);
    return CELL_OK;
}

Result _sys_prx_query_module(PpuThread&) { return CELL_OK; }

Result _sys_prx_register_library(PpuThread& t, u32 library)
{
    if (!t.kernel.impl().guest.valid(library, 0x1C)) return err(CELL_EFAULT);
    return CELL_OK; // the loader linked the module's libraries when it started it
}

Result _sys_prx_unregister_library(PpuThread&, u32) { return CELL_OK; }
Result _sys_prx_link_library(PpuThread&) { return CELL_OK; }
Result _sys_prx_unlink_library(PpuThread&) { return CELL_OK; }
Result _sys_prx_query_library(PpuThread&) { return CELL_OK; }
Result _sys_prx_start(PpuThread&) { return CELL_OK; }
Result _sys_prx_stop(PpuThread&) { return CELL_OK; }
Result sys_prx_get_ppu_guid(PpuThread&) { return CELL_OK; }

// The debugger's module lists: root's (see the top).
Result sys_prx_dbg_get_module_id_list(PpuThread& t) { return debugger_only(t, "sys_prx_dbg_get_module_id_list"); }
Result sys_prx_dbg_get_module_list(PpuThread& t) { return debugger_only(t, "sys_prx_dbg_get_module_list"); }
Result sys_prx_dbg_get_module_info(PpuThread& t) { return debugger_only(t, "sys_prx_dbg_get_module_info"); }

// ------------------------------------------------------------------------------------------------- overlays

Result overlay_load(PpuThread& t, std::string_view what)
{
    if (!t.kernel.config().ppc_seg) return err(CELL_ENOSYS);
    PS_LOG_ERROR(kPrxChan, "{}: overlay modules (at fixed addresses in the 0x30000000 segment) are not supported yet",
        what);
    return err(CELL_ENOEXEC);
}

Result sys_overlay_load_module(PpuThread& t, u32, u32 path, u64, u32)
{
    if (!t.kernel.config().ppc_seg) return err(CELL_ENOSYS);
    if (!path) return err(CELL_EFAULT);
    return overlay_load(t, "sys_overlay_load_module");
}

Result sys_overlay_load_module_by_fd(PpuThread& t, u32, s32 fd, u64 offset, u64, u32)
{
    if (!t.kernel.config().ppc_seg) return err(CELL_ENOSYS);
    if (static_cast<s64>(offset) < 0) return err(CELL_EINVAL);
    if (!fs_file_path(t.kernel, fd)) return err(CELL_EBADF);
    return overlay_load(t, "sys_overlay_load_module_by_fd");
}

Result sys_overlay_unload_module(PpuThread& t, u32)
{
    if (!t.kernel.config().ppc_seg) return err(CELL_ENOSYS);
    return err(CELL_ESRCH); // none is loaded
}

Result sys_overlay_get_module_list(PpuThread&) { return CELL_OK; }
Result sys_overlay_get_module_info(PpuThread&) { return CELL_OK; }
Result sys_overlay_get_module_info2(PpuThread&) { return CELL_OK; }
Result sys_overlay_get_sdk_version(PpuThread&) { return CELL_OK; }
Result sys_overlay_get_module_dbg_info(PpuThread&) { return CELL_OK; }

void register_prx_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 460, sys_prx_dbg_get_module_id_list);
    PS_LV2_BIND(table, 461, _sys_prx_get_module_id_by_address);
    PS_LV2_BIND(table, 463, _sys_prx_load_module_by_fd);
    PS_LV2_BIND(table, 464, _sys_prx_load_module_on_memcontainer_by_fd);
    PS_LV2_BIND(table, 465, _sys_prx_load_module_list);
    PS_LV2_BIND(table, 466, _sys_prx_load_module_list_on_memcontainer);
    PS_LV2_BIND(table, 467, sys_prx_get_ppu_guid);
    PS_LV2_BIND(table, 480, _sys_prx_load_module);
    PS_LV2_BIND(table, 481, _sys_prx_start_module);
    PS_LV2_BIND(table, 482, _sys_prx_stop_module);
    PS_LV2_BIND(table, 483, _sys_prx_unload_module);
    PS_LV2_BIND(table, 484, _sys_prx_register_module);
    PS_LV2_BIND(table, 485, _sys_prx_query_module);
    PS_LV2_BIND(table, 486, _sys_prx_register_library);
    PS_LV2_BIND(table, 487, _sys_prx_unregister_library);
    PS_LV2_BIND(table, 488, _sys_prx_link_library);
    PS_LV2_BIND(table, 489, _sys_prx_unlink_library);
    PS_LV2_BIND(table, 490, _sys_prx_query_library);
    PS_LV2_BIND(table, 492, sys_prx_dbg_get_module_list);
    PS_LV2_BIND(table, 493, sys_prx_dbg_get_module_info);
    PS_LV2_BIND(table, 494, _sys_prx_get_module_list);
    PS_LV2_BIND(table, 495, _sys_prx_get_module_info);
    PS_LV2_BIND(table, 496, _sys_prx_get_module_id_by_name);
    PS_LV2_BIND(table, 497, _sys_prx_load_module_on_memcontainer);
    PS_LV2_BIND(table, 498, _sys_prx_start);
    PS_LV2_BIND(table, 499, _sys_prx_stop);
    PS_LV2_BIND(table, 450, sys_overlay_load_module);
    PS_LV2_BIND(table, 451, sys_overlay_unload_module);
    PS_LV2_BIND(table, 452, sys_overlay_get_module_list);
    PS_LV2_BIND(table, 453, sys_overlay_get_module_info);
    PS_LV2_BIND(table, 454, sys_overlay_load_module_by_fd);
    PS_LV2_BIND(table, 455, sys_overlay_get_module_info2);
    PS_LV2_BIND(table, 456, sys_overlay_get_sdk_version);
    PS_LV2_BIND(table, 457, sys_overlay_get_module_dbg_info);
    PS_LV2_BIND(table, 458, sys_overlay_get_module_dbg_info);
}

} // namespace pspocket::lv2
