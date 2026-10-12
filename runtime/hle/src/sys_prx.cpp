// sysPrxForUser's module functions (D-159): a game loads, starts, stops and unloads PRX modules of its own (and
// firmware ones by path). References: RPCS3 rpcs3/Emu/Cell/Modules/sys_prx_.cpp (the library's wrappers: the "_lv2prx"
// lwmutex, the start and stop entries called on the caller's thread) and lv2/sys_prx.cpp (the syscalls behind them).
// The loader does the placing and linking (HleProcess::module_loader, runtime/loader RuntimeModules); this file runs
// the module's start and stop functions on the calling thread and keeps the order of the calls.

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>

#include "guest.hpp"
#include "prx.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/syscalls.hpp"

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
// its start runs here (module_prologue with module_start, or module_start, with args and argp: entryx, D-916 part 1),
// its result goes to *result and back to the syscall (cmd 2).
s32 sys_prx_start_module(PPUContext& ctx, u32 id, u32 args, u32 argp, u32 result, u64 /*flags*/, u32 /*opt*/)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!result || !guest_readable(t, result, 4)) return code(lv2::CELL_EINVAL);
    ModuleLoader* l = loader_of(ctx);
    if (!l) return code(kPrxNoLoader);
    std::lock_guard lock(HleProcess::current()->state<PrxCalls>().mutex);
    ModuleEntry entry;
    if (const u32 r = l->start(id, &entry); r != lv2::CELL_OK) return code(r);
    // Firmware modules loaded for it start first, as at boot (D-175).
    for (const ModuleEntry& dep : l->take_dependency_starts()) {
        const u32 r = static_cast<u32>(run_module_entry(ctx, dep, 0, 0));
        PS_LOG_INFO(kChan, "sys_prx_start_module({:#x}): firmware module_start {:#x} (prologue {:#x}) returned {:#x}", id,
            dep.entry, dep.wrapper, r);
    }
    const s32 res = static_cast<s32>(run_module_entry(ctx, entry, args, argp));
    ps_write32(t.kernel.base(), result, static_cast<u32>(res));
    l->started(id, res);
    PS_LOG_INFO(kChan, "sys_prx_start_module({:#x}): module_start {:#x} (prologue {:#x}) returned {:#x}", id,
        entry.entry, entry.wrapper, static_cast<u32>(res));
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
    ModuleEntry entry;
    if (const u32 r = l->stop(id, &entry); r != lv2::CELL_OK) return code(r);
    const s32 res = static_cast<s32>(run_module_entry(ctx, entry, args, argp));
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

// sys_prx_load_module_on_memcontainer(path, mem_ct, flags, pOpt): there are no memory containers for modules here;
// the module loads as with sys_prx_load_module.
s32 sys_prx_load_module_on_memcontainer(PPUContext& ctx, u32 path, u32 /*mem_ct*/, u64 flags, u32 /*opt*/)
{
    std::lock_guard lock(HleProcess::current()->state<PrxCalls>().mutex);
    return load(ctx, path, flags);
}

s32 sys_prx_load_module_list_on_memcontainer(PPUContext& ctx, s32 count, u32 path_list, u32 /*mem_ct*/, u64 flags,
    u32 opt, u32 id_list)
{
    return sys_prx_load_module_list(ctx, count, path_list, flags, opt, id_list);
}

// sys_prx_load_module_by_fd(fd, offset, flags, pOpt): a module `offset` bytes into an open file (one inside an
// archive), read through the file's path.
s32 sys_prx_load_module_by_fd(PPUContext& ctx, s32 fd, u64 offset, u64 flags, u32 /*opt*/)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (flags) return code(lv2::CELL_EINVAL);
    StackVar v(ctx, t.kernel.base(), 0x430); // the descriptor, then its path
    ps_write32(t.kernel.base(), v.addr(), static_cast<u32>(fd));
    if (const s32 r = lv2::sys_fs_test(t, 6, 0, v.addr(), 4, v.addr() + 0x10, 0x420); r != lv2::CELL_OK) return r;
    const std::string p = guest_string(t, v.addr() + 0x10, 0x420);
    ModuleLoader* l = loader_of(ctx);
    if (!l) return code(kPrxNoLoader);
    std::lock_guard lock(HleProcess::current()->state<PrxCalls>().mutex);
    u32 id = 0;
    const u32 r = l->load_at(p, offset, &id);
    if (r != lv2::CELL_OK) {
        PS_LOG_ERROR(kChan, "sys_prx_load_module_by_fd(fd {} = {}, offset {:#x}): {:#010x}", fd, p, offset, r);
        return code(r);
    }
    PS_LOG_INFO(kChan, "sys_prx_load_module_by_fd(fd {} = {}, offset {:#x}): id {:#x}", fd, p, offset, id);
    return code(id);
}

s32 sys_prx_load_module_on_memcontainer_by_fd(PPUContext& ctx, s32 fd, u64 offset, u32 /*mem_ct*/, u64 flags, u32 opt)
{
    return sys_prx_load_module_by_fd(ctx, fd, offset, flags, opt);
}

// RPCS3 _sys_prx_get_module_id_by_address: the module whose segments hold addr.
s32 sys_prx_get_module_id_by_address(PPUContext& ctx, u32 addr)
{
    ModuleLoader* l = loader_of(ctx);
    const u32 id = l ? l->id_by_address(addr) : 0;
    return id ? code(id) : code(kPrxErrorUnknownModule);
}

// The calling module: the one whose code the return address is in (RPCS3: by the LR).
s32 sys_prx_get_my_module_id(PPUContext& ctx) { return sys_prx_get_module_id_by_address(ctx, static_cast<u32>(ctx.lr)); }

// sys_prx_get_module_list(flags, sys_prx_get_module_list_t* info {u64 size; u32 max; u32 count; u32* idlist}).
s32 sys_prx_get_module_list(PPUContext& ctx, u64, u32 info)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!info || !guest_readable(t, info, 20)) return code(lv2::CELL_EINVAL);
    const u32 max = ps_read32(base, info + 8), idlist = ps_read32(base, info + 16);
    if (!idlist) return code(lv2::CELL_EINVAL);
    ModuleLoader* l = loader_of(ctx);
    const std::vector<u32> ids = l ? l->list() : std::vector<u32>{};
    u32 count = 0;
    for (; count < max && count < ids.size(); count++) {
        if (!guest_readable(t, idlist + 4 * count, 4)) return code(lv2::CELL_EFAULT);
        ps_write32(base, idlist + 4 * count, ids[count]);
    }
    ps_write32(base, info + 12, count);
    return lv2::CELL_OK;
}

// sys_prx_get_module_info(id, flags, sys_prx_module_info_t* info): RPCS3's _sys_prx_get_module_info. The info's
// size is 0x48 (or 0x58 with the exports and imports ranges); the file name and segments go where the caller says.
s32 sys_prx_get_module_info(PPUContext& ctx, u32 id, u64, u32 info)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!info) return code(lv2::CELL_EINVAL);
    if (!guest_readable(t, info, 0x48)) return code(lv2::CELL_EFAULT);
    const u64 size = ps_read64(base, info);
    if (size != 0x48 && size != 0x58) return code(lv2::CELL_EINVAL);
    if (size == 0x58 && !guest_readable(t, info, 0x58)) return code(lv2::CELL_EFAULT);
    ModuleLoader* l = loader_of(ctx);
    ModuleInfo m;
    if (!l || !l->info(id, m)) return code(kPrxErrorUnknownModule);
    std::memset(base + info + 8, 0, 30);
    std::memcpy(base + info + 8, m.name.data(), std::min<usize>(m.name.size(), 29));
    ps_write8(base, info + 0x26, m.version[0]);
    ps_write8(base, info + 0x27, m.version[1]);
    ps_write32(base, info + 0x28, m.attributes);
    ps_write32(base, info + 0x2C, m.start); // the descriptors (RPCS3 lv2_prx::start.addr())
    ps_write32(base, info + 0x30, m.stop);
    ps_write32(base, info + 0x34, static_cast<u32>(m.segments.size()));
    const u32 filename = ps_read32(base, info + 0x38), filename_size = ps_read32(base, info + 0x3C);
    if (filename && filename_size) {
        if (!guest_readable(t, filename, filename_size)) return code(lv2::CELL_EFAULT);
        const usize n = std::min<usize>(m.path.size(), filename_size - 1);
        std::memcpy(base + filename, m.path.data(), n);
        base[filename + n] = 0;
    }
    if (const u32 segments = ps_read32(base, info + 0x40)) {
        const u32 room = ps_read32(base, info + 0x44);
        u32 i = 0;
        for (; i < m.segments.size() && i < room; i++) {
            const u32 at = segments + 0x28 * i; // sys_prx_segment_info_t: base, filesz, memsz, index, type (u64 each)
            if (!guest_readable(t, at, 0x28)) return code(lv2::CELL_EFAULT);
            ps_write64(base, at + 0x00, m.segments[i].base);
            ps_write64(base, at + 0x08, m.segments[i].filesz);
            ps_write64(base, at + 0x10, m.segments[i].memsz);
            ps_write64(base, at + 0x18, i);
            ps_write64(base, at + 0x20, 1); // PT_LOAD
        }
        ps_write32(base, info + 0x44, i);
    }
    if (size == 0x58) {
        ps_write32(base, info + 0x48, m.exports.first);
        ps_write32(base, info + 0x4C, m.exports.second - m.exports.first);
        ps_write32(base, info + 0x50, m.imports.first);
        ps_write32(base, info + 0x54, m.imports.second - m.imports.first);
    }
    return lv2::CELL_OK;
}

// RPCS3's _sys_prx_unregister_library is a stub: CELL_OK.
s32 sys_prx_unregister_library(PPUContext&, u32 lib_entry)
{
    PS_LOG_WARN(kChan, "sys_prx_unregister_library({:#x}): nothing to do (RPCS3)", lib_entry);
    return lv2::CELL_OK;
}

} // namespace

namespace {

// The loader seen from lv2's PRX calls: the same loader, lock and steps as the sysPrxForUser functions above.
class KernelModules final : public lv2::ModuleHost {
public:
    explicit KernelModules(HleProcess& p) : p_(p) {}

    u32 load(std::string_view path, u64 offset, u32* id) override
    {
        ModuleLoader* l = p_.module_loader();
        if (!l) return kPrxNoLoader;
        std::lock_guard lock(p_.state<PrxCalls>().mutex);
        return offset ? l->load_at(path, offset, id) : l->load(path, id);
    }
    u32 start(lv2::PpuThread& t, u32 id, u32* entry, u32* entry2) override
    {
        ModuleLoader* l = p_.module_loader();
        if (!l) return kPrxNoLoader;
        std::lock_guard lock(p_.state<PrxCalls>().mutex);
        ModuleEntry e;
        if (const u32 r = l->start(id, &e); r != lv2::CELL_OK) return r;
        *entry = e.entry;
        *entry2 = e.wrapper;
        for (const ModuleEntry& dep : l->take_dependency_starts()) {
            const u32 r = static_cast<u32>(run_module_entry(*t.ctx, dep, 0, 0));
            PS_LOG_INFO(kChan, "_sys_prx_start_module({:#x}): firmware module_start {:#x} (prologue {:#x}) returned {:#x}",
                id, dep.entry, dep.wrapper, r);
        }
        return lv2::CELL_OK;
    }
    void started(u32 id, s32 result) override
    {
        if (ModuleLoader* l = p_.module_loader()) {
            std::lock_guard lock(p_.state<PrxCalls>().mutex);
            l->started(id, result);
        }
    }
    u32 stop(u32 id, u32* entry, u32* entry2) override
    {
        ModuleLoader* l = p_.module_loader();
        if (!l) return kPrxNoLoader;
        std::lock_guard lock(p_.state<PrxCalls>().mutex);
        ModuleEntry e;
        const u32 r = l->stop(id, &e);
        *entry = e.entry;
        *entry2 = e.wrapper;
        return r;
    }
    void stopped(u32 id, s32 result) override
    {
        if (ModuleLoader* l = p_.module_loader()) {
            std::lock_guard lock(p_.state<PrxCalls>().mutex);
            l->stopped(id, result);
        }
    }
    u32 unload(u32 id) override
    {
        ModuleLoader* l = p_.module_loader();
        if (!l) return kPrxNoLoader;
        std::lock_guard lock(p_.state<PrxCalls>().mutex);
        return l->unload(id);
    }
    u32 id_by_name(std::string_view name) override
    {
        ModuleLoader* l = p_.module_loader();
        return l ? l->id_by_name(name) : 0;
    }
    u32 id_by_address(u32 addr) override
    {
        ModuleLoader* l = p_.module_loader();
        return l ? l->id_by_address(addr) : 0;
    }
    std::vector<u32> list() override
    {
        ModuleLoader* l = p_.module_loader();
        return l ? l->list() : std::vector<u32>{};
    }
    s32 write_info(lv2::PpuThread& t, u32 id, u32 info) override { return sys_prx_get_module_info(*t.ctx, id, 0, info); }
    bool entries(u32 id, u32* start, u32* stop) override
    {
        ModuleLoader* l = p_.module_loader();
        ModuleInfo m;
        if (!l || !l->info(id, m)) return false;
        *start = m.start;
        *stop = m.stop;
        return true;
    }

private:
    HleProcess& p_;
};

} // namespace

std::unique_ptr<lv2::ModuleHost> make_kernel_module_host(HleProcess& process)
{
    return std::make_unique<KernelModules>(process);
}

s32 prx_load_module(PPUContext& ctx, u32 path, u64 flags) { return sys_prx_load_module(ctx, path, flags, 0); }

s32 prx_start_module(PPUContext& ctx, u32 id, u32 args, u32 argp, u32 result)
{
    return sys_prx_start_module(ctx, id, args, argp, result, 0, 0);
}

s32 prx_stop_module(PPUContext& ctx, u32 id, u32 args, u32 argp, u32 result)
{
    return sys_prx_stop_module(ctx, id, args, argp, result, 0, 0);
}

s32 prx_unload_module(PPUContext& ctx, u32 id) { return sys_prx_unload_module(ctx, id, 0, 0); }

void register_sys_prx(Registry& r)
{
    r.add("sysPrxForUser", {
        PS_HLE_FUNC(0x26090058, sys_prx_load_module),
        PS_HLE_FUNC(0xB27C8AE7, sys_prx_load_module_list),
        PS_HLE_FUNC(0x9F18429D, sys_prx_start_module),
        PS_HLE_FUNC(0x80FB0C19, sys_prx_stop_module),
        PS_HLE_FUNC(0xF0AECE0D, sys_prx_unload_module),
        PS_HLE_FUNC(0xE0998DBF, sys_prx_get_module_id_by_name),
        PS_HLE_FUNC(0xAA6D9BFF, sys_prx_load_module_on_memcontainer),
        PS_HLE_FUNC(0xE7EF3A80, sys_prx_load_module_list_on_memcontainer),
        PS_HLE_FUNC(0xEF68C17C, sys_prx_load_module_by_fd),
        PS_HLE_FUNC(0xA330AD84, sys_prx_load_module_on_memcontainer_by_fd),
        PS_HLE_FUNC(0x0341BB97, sys_prx_get_module_id_by_address),
        PS_HLE_FUNC(0x74311398, sys_prx_get_my_module_id),
        PS_HLE_FUNC(0xA5D06BF0, sys_prx_get_module_list),
        PS_HLE_FUNC(0x84BB6774, sys_prx_get_module_info),
        PS_HLE_FUNC(0xD0EA47A7, sys_prx_unregister_library),
    });
}

} // namespace pspocket::hle
