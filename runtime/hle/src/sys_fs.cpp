// sys_fs (libfs): the cellFs file API over lv2's file system syscalls. Reference: RPCS3
// rpcs3/Emu/Cell/Modules/cellFs.cpp, where most functions forward to the syscall; the read/write-with-offset and
// allocate-area functions go through sys_fs_fcntl with the same argument blocks.
//
// Opens and stats are logged with their path and result (info; failures as warnings), so a boot log shows which
// files the game asks for.

#include <format>
#include <string>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/syscalls.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sys_fs";
constexpr s32 CELL_FS_O_CREAT = 0100;
constexpr s32 CELL_FS_S_IRUSR = 0400, CELL_FS_S_IWUSR = 0200;

s32 error(lv2::CellError e) { return lv2::err(e); }

// A guest path for the log ("?" when unreadable).
std::string path_of(PPUContext& ctx, u32 addr)
{
    lv2::PpuThread& t = thread_of(ctx);
    std::string out;
    for (u32 i = 0; i < 1024; i++) {
        if (!guest_readable(t, addr + i, 1)) return out.empty() ? "?" : out;
        const char c = static_cast<char>(ps_read8(t.kernel.base(), addr + i));
        if (!c) break;
        out += c;
    }
    return out;
}

void log_result(PPUContext& ctx, const char* what, u32 path, s32 r, std::string_view extra = {})
{
    if (r == lv2::CELL_OK) {
        PS_LOG_INFO(kChan, "{}({}){}", what, path_of(ctx, path), extra);
    } else {
        PS_LOG_WARN(kChan, "{}({}) -> {}", what, path_of(ctx, path), lv2::error_name(r));
    }
}

s32 cellFsOpen(PPUContext& ctx, u32 path, s32 flags, u32 fd, u32 arg, u64 size)
{
    if (!fd) return error(lv2::CELL_EFAULT);
    lv2::PpuThread& t = thread_of(ctx);
    const s32 r = lv2::sys_fs_open(t, path, flags, fd, (flags & CELL_FS_O_CREAT) ? CELL_FS_S_IRUSR | CELL_FS_S_IWUSR : 0,
        arg, size);
    log_result(ctx, "cellFsOpen", path, r,
        r == lv2::CELL_OK ? std::format(", flags {:#o} -> fd {}", flags, ps_read32(t.kernel.base(), fd)) : std::string());
    return r;
}

// A missing count pointer gets a stack variable, as RPCS3 passes a vm::var.
s32 cellFsRead(PPUContext& ctx, u32 fd, u32 buf, u64 nbytes, u32 nread)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (nread) return lv2::sys_fs_read(t, fd, buf, nbytes, nread);
    StackVar n(ctx, t.kernel.base(), 8);
    return lv2::sys_fs_read(t, fd, buf, nbytes, n.addr());
}

s32 cellFsWrite(PPUContext& ctx, u32 fd, u32 buf, u64 nbytes, u32 nwrite)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (nwrite) return lv2::sys_fs_write(t, fd, buf, nbytes, nwrite);
    StackVar n(ctx, t.kernel.base(), 8);
    return lv2::sys_fs_write(t, fd, buf, nbytes, n.addr());
}

s32 cellFsClose(PPUContext& ctx, u32 fd) { return lv2::sys_fs_close(thread_of(ctx), fd); }

s32 cellFsOpendir(PPUContext& ctx, u32 path, u32 fd)
{
    if (!fd) return error(lv2::CELL_EFAULT);
    const s32 r = lv2::sys_fs_opendir(thread_of(ctx), path, fd);
    log_result(ctx, "cellFsOpendir", path, r);
    return r;
}

s32 cellFsReaddir(PPUContext& ctx, u32 fd, u32 dir, u32 nread)
{
    if (!dir || !nread) return error(lv2::CELL_EFAULT);
    return lv2::sys_fs_readdir(thread_of(ctx), fd, dir, nread);
}

s32 cellFsClosedir(PPUContext& ctx, u32 fd) { return lv2::sys_fs_closedir(thread_of(ctx), fd); }

s32 cellFsStat(PPUContext& ctx, u32 path, u32 sb)
{
    if (!sb) return error(lv2::CELL_EFAULT);
    const s32 r = lv2::sys_fs_stat(thread_of(ctx), path, sb);
    log_result(ctx, "cellFsStat", path, r);
    return r;
}

s32 cellFsMkdir(PPUContext& ctx, u32 path, s32 mode)
{
    const s32 r = lv2::sys_fs_mkdir(thread_of(ctx), path, mode);
    log_result(ctx, "cellFsMkdir", path, r);
    return r;
}

s32 cellFsRename(PPUContext& ctx, u32 from, u32 to) { return lv2::sys_fs_rename(thread_of(ctx), from, to); }
s32 cellFsChmod(PPUContext& ctx, u32 path, s32 mode) { return lv2::sys_fs_chmod(thread_of(ctx), path, mode); }
s32 cellFsFsync(PPUContext& ctx, u32 fd) { return lv2::sys_fs_fsync(thread_of(ctx), fd); }
s32 cellFsRmdir(PPUContext& ctx, u32 path) { return lv2::sys_fs_rmdir(thread_of(ctx), path); }
s32 cellFsUnlink(PPUContext& ctx, u32 path) { return lv2::sys_fs_unlink(thread_of(ctx), path); }
s32 cellFsFtruncate(PPUContext& ctx, u32 fd, u64 size) { return lv2::sys_fs_ftruncate(thread_of(ctx), fd, size); }

s32 cellFsLseek(PPUContext& ctx, u32 fd, s64 offset, u32 whence, u32 pos)
{
    if (!pos) return error(lv2::CELL_EFAULT);
    return lv2::sys_fs_lseek(thread_of(ctx), fd, offset, static_cast<s32>(whence), pos);
}

s32 cellFsUtime(PPUContext& ctx, u32 path, u32 timep)
{
    if (!timep) return error(lv2::CELL_EFAULT);
    return lv2::sys_fs_utime(thread_of(ctx), path, timep);
}

// RPCS3 cellFsReadWithOffset / cellFsWriteWithOffset: sys_fs_fcntl 0x8000000A/B with an lv2_file_op_rw block (its
// vtable "intentionally wrong", as in RPCS3). The count is 0 when the call fails (other than CELL_EFSSPECIFIC), and
// an operation that ran but failed returns its out_code.
s32 rw_with_offset(PPUContext& ctx, u32 op, u32 vtable, u32 fd, u64 offset, u32 buf, u64 size, u32 count)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (fd - 3 > 252) {
        if (count) ps_write64(base, count, 0);
        return error(lv2::CELL_EBADF);
    }
    StackVar arg(ctx, base, 0x38);
    ps_write32(base, arg.addr() + 0x00, vtable);
    ps_write32(base, arg.addr() + 0x04, op);
    ps_write32(base, arg.addr() + 0x10, fd);
    ps_write32(base, arg.addr() + 0x14, buf);
    ps_write64(base, arg.addr() + 0x18, offset);
    ps_write64(base, arg.addr() + 0x20, size);
    const s32 r = lv2::sys_fs_fcntl(t, fd, op, arg.addr(), 0x38);
    if (count) {
        ps_write64(base, count,
            r != lv2::CELL_OK && static_cast<u32>(r) != lv2::CELL_EFSSPECIFIC ? 0 : ps_read64(base, arg.addr() + 0x30));
    }
    if (r == lv2::CELL_OK && ps_read32(base, arg.addr() + 0x28)) return static_cast<s32>(ps_read32(base, arg.addr() + 0x28));
    return r;
}

s32 cellFsReadWithOffset(PPUContext& ctx, u32 fd, u64 offset, u32 buf, u64 buffer_size, u32 nread)
{
    return rw_with_offset(ctx, 0x8000000A, 0xFA8A0000, fd, offset, buf, buffer_size, nread);
}

s32 cellFsWriteWithOffset(PPUContext& ctx, u32 fd, u64 offset, u32 buf, u64 data_size, u32 nwrite)
{
    if (!buf) {
        if (nwrite) ps_write64(thread_of(ctx).kernel.base(), nwrite, 0);
        return error(lv2::CELL_EFAULT);
    }
    return rw_with_offset(ctx, 0x8000000B, 0xFA8B0000, fd, offset, buf, data_size, nwrite);
}

// RPCS3 cellFsAllocateFileAreaWithoutZeroFill: sys_fs_fcntl 0xE0000017 (lv2_file_e0000017), which sets the size.
s32 cellFsAllocateFileAreaWithoutZeroFill(PPUContext& ctx, u32 path, u64 size)
{
    if (!path) return error(lv2::CELL_EFAULT);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    StackVar ctrl(ctx, base, 0x28);
    ps_write32(base, ctrl.addr() + 0x00, 0x28);
    ps_write32(base, ctrl.addr() + 0x04, 0x10);
    ps_write32(base, ctrl.addr() + 0x08, 0x20);
    ps_write32(base, ctrl.addr() + 0x10, path);
    ps_write64(base, ctrl.addr() + 0x18, size);
    ps_write32(base, ctrl.addr() + 0x20, lv2::CELL_ENOSYS);
    if (const s32 r = lv2::sys_fs_fcntl(t, 0xFFFFFFFF, 0xE0000017, ctrl.addr(), 0x28); r != lv2::CELL_OK) return r;
    return static_cast<s32>(ps_read32(base, ctrl.addr() + 0x20));
}

} // namespace

void register_sys_fs(Registry& r)
{
    r.add("sys_fs", {
        PS_HLE_FUNC(0x718BF5F8, cellFsOpen),
        PS_HLE_FUNC(0x4D5FF8E2, cellFsRead),
        PS_HLE_FUNC(0xECDCF2AB, cellFsWrite),
        PS_HLE_FUNC(0x2CB51F0D, cellFsClose),
        PS_HLE_FUNC(0x3F61245C, cellFsOpendir),
        PS_HLE_FUNC(0x5C74903D, cellFsReaddir),
        PS_HLE_FUNC(0xFF42DCC3, cellFsClosedir),
        PS_HLE_FUNC(0x7DE6DCED, cellFsStat),
        PS_HLE_FUNC(0xBA901FE6, cellFsMkdir),
        PS_HLE_FUNC(0xF12EECC8, cellFsRename),
        PS_HLE_FUNC(0x99406D0B, cellFsChmod),
        PS_HLE_FUNC(0x967A162B, cellFsFsync),
        PS_HLE_FUNC(0x2796FDF3, cellFsRmdir),
        PS_HLE_FUNC(0x7F4677A8, cellFsUnlink),
        PS_HLE_FUNC(0xA397D042, cellFsLseek),
        PS_HLE_FUNC(0x0E2939E5, cellFsFtruncate),
        PS_HLE_FUNC(0xBEF554A4, cellFsUtime),
        PS_HLE_FUNC(0x0D5B4A14, cellFsReadWithOffset),
        PS_HLE_FUNC(0x8CB722D5, cellFsWriteWithOffset),
        PS_HLE_FUNC(0x7A0329A1, cellFsAllocateFileAreaWithoutZeroFill),
    });
}

} // namespace pspocket::hle
