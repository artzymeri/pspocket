// sys_fs (libfs): the cellFs file API over lv2's file system syscalls. Reference: RPCS3
// rpcs3/Emu/Cell/Modules/cellFs.cpp/.h, where most functions forward to the syscall; the read/write-with-offset and
// allocate-area functions go through sys_fs_fcntl with the same argument blocks.
//
// Opens and stats are logged with their path and result (info; failures as warnings), so a boot log shows which
// files the game asks for.
//
// Encrypted SDATA files (cellFsSdataOpen, and sys_fs_open's SDATA type argument) are decrypted by the kernel through
// the NPD opener this module installs in every process (install_npd_opener, D-165): recomp::NpdFile with the build's
// keys, as RPCS3's lv2 sys_fs_open wraps them in an EDATADecrypter.
//
// cellFsSdataOpenByFd opens an SDATA file inside an open file (an MSELF archive) the same way, through sys_fs_fcntl
// 0x80000009. Stream reads (cellFsSt*) are pspocket's own: RPCS3 leaves them to Sony's libfs (see "stream reads").
//
// Asynchronous reads and writes (cellFsAio*) follow RPCS3's fs_aio_thread, which its HLE never starts (it logs "use LLE", for
// the firmware's libfs): the first cellFsAioInit creates the AIO thread, an lv2 PPU thread whose entry is an internal
// HLE function. cellFsAioRead queues a request and returns its ID at once; the AIO thread reads at the request's
// offset without moving the file position, then calls the game's callback with the result, on itself.

#include <algorithm>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <string>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/npd.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sys_fs";
constexpr std::string_view kModule = "sys_fs";
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

// cellFsSdataOpen(path, flags, fd, arg, size): an encrypted SDATA file, read-only. As RPCS3: other flags are
// CELL_EINVAL; the open is sys_fs_open's with the SDATA type argument {0x180, 0x10} (the caller's argument is not
// used), so the kernel decrypts it.
s32 cellFsSdataOpen(PPUContext& ctx, u32 path, s32 flags, u32 fd, u32 /*arg*/, u64 /*size*/)
{
    if (flags != 0) return error(lv2::CELL_EINVAL); // CELL_FS_O_RDONLY
    if (!fd) return error(lv2::CELL_EFAULT);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    StackVar type(ctx, base, 8);
    ps_write32(base, type.addr(), 0x180);
    ps_write32(base, type.addr() + 4, 0x10);
    const s32 r = lv2::sys_fs_open(t, path, 0, fd, 0, type.addr(), 8);
    log_result(ctx, "cellFsSdataOpen", path, r,
        r == lv2::CELL_OK ? std::format(" -> fd {}", ps_read32(base, fd)) : std::string());
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

// RPCS3 cellFsFstat: sys_fs_fstat as it is.
s32 cellFsFstat(PPUContext& ctx, u32 fd, u32 sb) { return lv2::sys_fs_fstat(thread_of(ctx), fd, sb); }

// RPCS3 cellFsTruncate and cellFsFdatasync: the syscalls.
s32 cellFsTruncate(PPUContext& ctx, u32 path, u64 size) { return lv2::sys_fs_truncate(thread_of(ctx), path, size); }
s32 cellFsFdatasync(PPUContext& ctx, u32 fd) { return lv2::sys_fs_fdatasync(thread_of(ctx), fd); }

// cellFsGetBlockSize(path, u64* sector_size, u64* block_size) / cellFsFGetBlockSize(fd, ...): RPCS3's pointer checks,
// then the syscalls with a scratch for their extra outputs.
s32 cellFsGetBlockSize(PPUContext& ctx, u32 path, u32 sector_size, u32 block_size)
{
    if (!path || !sector_size || !block_size) return error(lv2::CELL_EFAULT);
    lv2::PpuThread& t = thread_of(ctx);
    StackVar arg4(ctx, t.kernel.base(), 8);
    return lv2::sys_fs_get_block_size(t, path, sector_size, block_size, arg4.addr());
}

s32 cellFsFGetBlockSize(PPUContext& ctx, u32 fd, u32 sector_size, u32 block_size)
{
    if (!sector_size || !block_size) return error(lv2::CELL_EFAULT);
    lv2::PpuThread& t = thread_of(ctx);
    StackVar scratch(ctx, t.kernel.base(), 16); // u64 arg4, s32 flags
    return lv2::sys_fs_fget_block_size(t, fd, sector_size, block_size, scratch.addr(), scratch.addr() + 8);
}

// cellFsGetPath(fd, char* out_path): sys_fs_test(6) with room for 0x420 bytes (RPCS3).
s32 cellFsGetPath(PPUContext& ctx, u32 fd, u32 out_path)
{
    if (!out_path) return error(lv2::CELL_EFAULT);
    lv2::PpuThread& t = thread_of(ctx);
    StackVar fdv(ctx, t.kernel.base(), 4);
    ps_write32(t.kernel.base(), fdv.addr(), fd);
    return lv2::sys_fs_test(t, 6, 0, fdv.addr(), 4, out_path, 0x420);
}

// cellFsGetDirectoryEntries(fd, CellFsDirectoryEntry* entries, entries_size, u32* data_count): one entry a call, through
// sys_fs_fcntl 0xE0000012 with RPCS3's argument block (lv2_file_op_dir: vtable, op, -, then dir_info).
s32 cellFsGetDirectoryEntries(PPUContext& ctx, u32 fd, u32 entries, u32 entries_size, u32 data_count)
{
    if (!data_count || !entries) return error(lv2::CELL_EFAULT);
    if (fd - 3 > 252) return error(lv2::CELL_EBADF);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, data_count, 4)) return error(lv2::CELL_EFAULT);
    StackVar op(ctx, base, 0x1C);
    const u32 info = op.addr() + 0xC;
    ps_write32(base, op.addr() + 0, 0xFAE12000); // RPCS3: a vtable no one reads
    ps_write32(base, op.addr() + 4, 0xE0000012);
    ps_write32(base, info + 8, entries);
    ps_write32(base, info + 12, entries_size / lv2::kCellFsDirectoryEntrySize);
    const s32 r = lv2::sys_fs_fcntl(t, fd, 0xE0000012, info, 0x10);
    ps_write32(base, data_count, ps_read32(base, info + 4));
    if (r == lv2::CELL_OK && ps_read32(base, info)) return static_cast<s32>(ps_read32(base, info));
    return r;
}

// cellFsSdataOpenByFd(mself_fd, flags, u32* sdata_fd, offset, arg, size): an SDATA file inside an open MSELF archive,
// through sys_fs_fcntl 0x80000009 with RPCS3's argument block. *sdata_fd is -1 unless it opens.
s32 cellFsSdataOpenByFd(PPUContext& ctx, u32 mself_fd, s32 flags, u32 sdata_fd, u64 offset, u32 arg, u64 size)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!sdata_fd || !guest_readable(t, sdata_fd, 4)) return error(lv2::CELL_EFAULT);
    ps_write32(base, sdata_fd, 0xFFFFFFFF);
    if (mself_fd < 3 || mself_fd > 255) return error(lv2::CELL_EBADF);
    if (flags) return error(lv2::CELL_EINVAL);
    StackVar ctrl(ctx, base, 0x40);
    ps_write32(base, ctrl.addr() + 0x00, 0xFA880000); // RPCS3: vtables no one reads
    ps_write32(base, ctrl.addr() + 0x04, 0x80000009);
    ps_write32(base, ctrl.addr() + 0x10, mself_fd);
    ps_write64(base, ctrl.addr() + 0x18, offset);
    ps_write32(base, ctrl.addr() + 0x20, 0xFA880020);
    ps_write32(base, ctrl.addr() + 0x24, 0x180);
    ps_write32(base, ctrl.addr() + 0x28, 0x10);
    ps_write32(base, ctrl.addr() + 0x2C, static_cast<u32>(size));
    ps_write32(base, ctrl.addr() + 0x30, arg);
    if (const s32 r = lv2::sys_fs_fcntl(t, mself_fd, 0x80000009, ctrl.addr(), 0x40)) return r;
    if (const s32 r = static_cast<s32>(ps_read32(base, ctrl.addr() + 0x38))) return r;
    ps_write32(base, sdata_fd, ps_read32(base, ctrl.addr() + 0x3C));
    PS_LOG_INFO(kChan, "cellFsSdataOpenByFd(fd {}, offset {:#x}): fd {}", mself_fd, offset, ps_read32(base, sdata_fd));
    return lv2::CELL_OK;
}

// cellFsSetIoBufferFromDefaultContainer(fd, buffer_size, page_type): an open file is enough (RPCS3; there is no I/O
// buffer to size here).
s32 cellFsSetIoBufferFromDefaultContainer(PPUContext& ctx, u32 fd, u32, u32)
{
    lv2::PpuThread& t = thread_of(ctx);
    StackVar sb(ctx, t.kernel.base(), lv2::kCellFsStatSize);
    return lv2::sys_fs_fstat(t, fd, sb.addr()) == error(lv2::CELL_EBADF) ? error(lv2::CELL_EBADF) : lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- stream reads
//
// cellFsSt*: libfs reads a file ahead into a ring buffer on a thread of its own, from cellFsStReadStart(offset, size)
// on, and the game takes the data with cellFsStRead (a copy) or cellFsStReadGetCurrentAddr / PutCurrentAddr (in place,
// the "copyless" mode). RPCS3 leaves all of it a TODO, because it runs Sony's libfs (LLE) by default; libfs is native
// here, so pspocket serves streams itself. Reads go straight to the file, so data is always there when the game asks:
// cellFsStRead reads at the stream's position, GetCurrentAddr reads the next block (block_size bytes at most) into
// the ring buffer the library allocated at cellFsStReadInit, cellFsStReadWait returns at once, and
// cellFsStReadWaitCallback calls its callback(fd, size) before it returns. The stream ends at offset + size (size 0:
// at the end of the file); reads past it give 0 bytes. A stream function on a file without cellFsStReadInit, or a
// read before cellFsStReadStart, is CELL_ENXIO, the answer cellFsAioRead gives without its thread.

constexpr u32 CELL_FS_ST_COPYLESS = 1;
constexpr u64 CELL_FS_ST_INITIALIZED = 0x0001, CELL_FS_ST_NOT_INITIALIZED = 0x0002, CELL_FS_ST_STOP = 0x0100,
              CELL_FS_ST_PROGRESS = 0x0200;
constexpr u32 kRingBufferSize = 28; // CellFsRingBuffer: be64 ringbuf_size, block_size, transfer_rate; be32 copy

struct Stream {
    u64 ringbuf_size = 0, block_size = 0, transfer_rate = 0;
    u32 copy = 0;
    u32 ring = 0; // the ring buffer, from the main area
    bool started = false;
    u64 pos = 0, end = 0; // the next byte to deliver, and the stream's end
    u64 lent = 0;         // copyless: bytes handed out by GetCurrentAddr and not yet put back
};

struct StreamState {
    std::mutex mutex;
    std::map<u32, Stream> streams; // by fd
};

StreamState& streams() { return HleProcess::current()->state<StreamState>(); }

// An open file (EBADF otherwise), and its size.
s32 open_file(PPUContext& ctx, u32 fd, u64* size = nullptr, s32* flags = nullptr)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    StackVar sb(ctx, base, lv2::kCellFsStatSize);
    const s32 r = lv2::sys_fs_fstat(t, fd, sb.addr());
    if (r != lv2::CELL_OK) return error(lv2::CELL_EBADF);
    if (size) *size = ps_read64(base, sb.addr() + 36);
    if (flags) {
        StackVar scratch(ctx, base, 28); // sector, block, arg4 (u64 each), then the flags
        lv2::sys_fs_fget_block_size(t, fd, scratch.addr(), scratch.addr() + 8, scratch.addr() + 16, scratch.addr() + 24);
        *flags = static_cast<s32>(ps_read32(base, scratch.addr() + 24));
    }
    return lv2::CELL_OK;
}

s32 cellFsStReadInit(PPUContext& ctx, u32 fd, u32 ringbuf)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!ringbuf || !guest_readable(t, ringbuf, kRingBufferSize)) return error(lv2::CELL_EFAULT);
    u8* base = t.kernel.base();
    Stream st;
    st.ringbuf_size = ps_read64(base, ringbuf);
    st.block_size = ps_read64(base, ringbuf + 8);
    st.transfer_rate = ps_read64(base, ringbuf + 16);
    st.copy = ps_read32(base, ringbuf + 24);
    // RPCS3's checks: the copy mode, a block a multiple of 4 KB, a ring a multiple of the block.
    if (st.copy & ~CELL_FS_ST_COPYLESS) return error(lv2::CELL_EINVAL);
    if (st.block_size & 0xFFF) return error(lv2::CELL_EINVAL);
    if (!st.block_size || st.ringbuf_size % st.block_size) return error(lv2::CELL_EINVAL);
    s32 flags = 0;
    if (const s32 r = open_file(ctx, fd, nullptr, &flags); r != lv2::CELL_OK) return r;
    if (flags & 1) return error(lv2::CELL_EPERM); // CELL_FS_O_WRONLY
    StreamState& ss = streams();
    std::lock_guard lock(ss.mutex);
    if (const auto it = ss.streams.find(fd); it != ss.streams.end() && it->second.ring) {
        HleProcess::current()->main_memory().free(it->second.ring);
    }
    st.ring = HleProcess::current()->main_memory().alloc(static_cast<u32>(st.ringbuf_size));
    if (!st.ring) return error(lv2::CELL_ENOMEM);
    PS_LOG_INFO(kChan, "cellFsStReadInit(fd {}): ring {:#x} bytes at {:#010x}, blocks of {:#x}, {}", fd,
        st.ringbuf_size, st.ring, st.block_size, st.copy ? "copyless" : "copy");
    ss.streams[fd] = st;
    return lv2::CELL_OK;
}

// The stream of fd, the state locked: EBADF for a descriptor that isn't an open file, ENXIO without cellFsStReadInit.
Stream* stream_of(PPUContext& ctx, StreamState& ss, u32 fd, s32& r)
{
    r = open_file(ctx, fd);
    if (r != lv2::CELL_OK) return nullptr;
    const auto it = ss.streams.find(fd);
    if (it == ss.streams.end()) {
        r = error(lv2::CELL_ENXIO);
        return nullptr;
    }
    return &it->second;
}

s32 cellFsStReadFinish(PPUContext& ctx, u32 fd)
{
    if (const s32 r = open_file(ctx, fd); r != lv2::CELL_OK) return r;
    StreamState& ss = streams();
    std::lock_guard lock(ss.mutex);
    if (const auto it = ss.streams.find(fd); it != ss.streams.end()) {
        if (it->second.ring) HleProcess::current()->main_memory().free(it->second.ring);
        ss.streams.erase(it);
    }
    return lv2::CELL_OK;
}

s32 cellFsStReadGetRingBuf(PPUContext& ctx, u32 fd, u32 ringbuf)
{
    StreamState& ss = streams();
    std::lock_guard lock(ss.mutex);
    s32 r;
    Stream* st = stream_of(ctx, ss, fd, r);
    if (!st) return r;
    if (!ringbuf || !guest_readable(thread_of(ctx), ringbuf, kRingBufferSize)) return error(lv2::CELL_EFAULT);
    u8* base = guest_base(ctx);
    ps_write64(base, ringbuf, st->ringbuf_size);
    ps_write64(base, ringbuf + 8, st->block_size);
    ps_write64(base, ringbuf + 16, st->transfer_rate);
    ps_write32(base, ringbuf + 24, st->copy);
    return lv2::CELL_OK;
}

s32 cellFsStReadGetStatus(PPUContext& ctx, u32 fd, u32 status)
{
    if (const s32 r = open_file(ctx, fd); r != lv2::CELL_OK) return r;
    if (!status || !guest_readable(thread_of(ctx), status, 8)) return error(lv2::CELL_EFAULT);
    StreamState& ss = streams();
    std::lock_guard lock(ss.mutex);
    const auto it = ss.streams.find(fd);
    const u64 value = it == ss.streams.end() ? CELL_FS_ST_NOT_INITIALIZED
                                             : CELL_FS_ST_INITIALIZED |
                                                   (it->second.started ? CELL_FS_ST_PROGRESS : CELL_FS_ST_STOP);
    ps_write64(guest_base(ctx), status, value);
    return lv2::CELL_OK;
}

// cellFsStReadGetRegid(fd, u64* regid): the stream's region; pspocket's streams have no regions (0).
s32 cellFsStReadGetRegid(PPUContext& ctx, u32 fd, u32 regid)
{
    StreamState& ss = streams();
    std::lock_guard lock(ss.mutex);
    s32 r;
    if (!stream_of(ctx, ss, fd, r)) return r;
    if (!regid || !guest_readable(thread_of(ctx), regid, 8)) return error(lv2::CELL_EFAULT);
    ps_write64(guest_base(ctx), regid, 0);
    return lv2::CELL_OK;
}

s32 cellFsStReadStart(PPUContext& ctx, u32 fd, u64 offset, u64 size)
{
    u64 file_size = 0;
    if (const s32 r = open_file(ctx, fd, &file_size); r != lv2::CELL_OK) return r;
    StreamState& ss = streams();
    std::lock_guard lock(ss.mutex);
    s32 r;
    Stream* st = stream_of(ctx, ss, fd, r);
    if (!st) return r;
    st->started = true;
    st->pos = offset;
    st->end = size ? offset + size : file_size;
    st->lent = 0;
    PS_LOG_DEBUG(kChan, "cellFsStReadStart(fd {}, offset {:#x}, size {:#x})", fd, offset, size);
    return lv2::CELL_OK;
}

s32 cellFsStReadStop(PPUContext& ctx, u32 fd)
{
    StreamState& ss = streams();
    std::lock_guard lock(ss.mutex);
    s32 r;
    Stream* st = stream_of(ctx, ss, fd, r);
    if (!st) return r;
    st->started = false;
    return lv2::CELL_OK;
}

// The started stream of fd (ENXIO before cellFsStReadStart).
Stream* started_stream(PPUContext& ctx, StreamState& ss, u32 fd, s32& r)
{
    Stream* st = stream_of(ctx, ss, fd, r);
    if (st && !st->started) {
        r = error(lv2::CELL_ENXIO);
        return nullptr;
    }
    return st;
}

// cellFsStRead(fd, buf, size, u64* rsize): up to size bytes from the stream's position.
s32 cellFsStRead(PPUContext& ctx, u32 fd, u32 buf, u64 size, u32 rsize)
{
    StreamState& ss = streams();
    std::lock_guard lock(ss.mutex);
    s32 r;
    Stream* st = started_stream(ctx, ss, fd, r);
    if (!st) return r;
    if (!rsize || !guest_readable(thread_of(ctx), rsize, 8)) return error(lv2::CELL_EFAULT);
    const u64 want = std::min(size, st->end > st->pos ? st->end - st->pos : 0);
    StackVar count(ctx, guest_base(ctx), 8);
    ps_write64(guest_base(ctx), count.addr(), 0);
    if (want) {
        r = rw_with_offset(ctx, 0x8000000A, 0xFA8A0000, fd, st->pos, buf, want, count.addr());
        if (r != lv2::CELL_OK) return r;
    }
    const u64 done = ps_read64(guest_base(ctx), count.addr());
    st->pos += done;
    ps_write64(guest_base(ctx), rsize, done);
    return lv2::CELL_OK;
}

// cellFsStReadGetCurrentAddr(fd, u32* addr, u64* size): the next block, read into the ring buffer, in place.
s32 cellFsStReadGetCurrentAddr(PPUContext& ctx, u32 fd, u32 addr, u32 size)
{
    StreamState& ss = streams();
    std::lock_guard lock(ss.mutex);
    s32 r;
    Stream* st = started_stream(ctx, ss, fd, r);
    if (!st) return r;
    lv2::PpuThread& t = thread_of(ctx);
    if (!addr || !size || !guest_readable(t, addr, 4) || !guest_readable(t, size, 8)) return error(lv2::CELL_EFAULT);
    const u64 at = st->pos + st->lent;
    const u64 want = std::min(st->block_size, st->end > at ? st->end - at : 0);
    StackVar count(ctx, guest_base(ctx), 8);
    ps_write64(guest_base(ctx), count.addr(), 0);
    if (want) {
        r = rw_with_offset(ctx, 0x8000000A, 0xFA8A0000, fd, at, st->ring, want, count.addr());
        if (r != lv2::CELL_OK) return r;
    }
    const u64 done = ps_read64(guest_base(ctx), count.addr());
    st->lent += done;
    ps_write32(guest_base(ctx), addr, st->ring);
    ps_write64(guest_base(ctx), size, done);
    return lv2::CELL_OK;
}

// cellFsStReadPutCurrentAddr(fd, addr, size): the game is done with size bytes of what it was lent.
s32 cellFsStReadPutCurrentAddr(PPUContext& ctx, u32 fd, u32, u64 size)
{
    StreamState& ss = streams();
    std::lock_guard lock(ss.mutex);
    s32 r;
    Stream* st = started_stream(ctx, ss, fd, r);
    if (!st) return r;
    const u64 back = std::min(size, st->lent);
    st->pos += back;
    st->lent = 0;
    return lv2::CELL_OK;
}

// The data is always there: the waits return at once (the callback runs before cellFsStReadWaitCallback returns).
s32 cellFsStReadWait(PPUContext& ctx, u32 fd, u64)
{
    StreamState& ss = streams();
    std::lock_guard lock(ss.mutex);
    s32 r;
    return started_stream(ctx, ss, fd, r) ? lv2::CELL_OK : r;
}

s32 cellFsStReadWaitCallback(PPUContext& ctx, u32 fd, u64 size, u32 func)
{
    {
        StreamState& ss = streams();
        std::lock_guard lock(ss.mutex);
        s32 r;
        if (!started_stream(ctx, ss, fd, r)) return r;
    }
    if (func) call_guest(ctx, func, {fd, size});
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- free space

constexpr u64 kGiB = 1ull << 30;
constexpr u32 kHdd0BlockSize = 4096; // RPCS3's lv2_fs_mount_point table: /dev_hdd0's block size
constexpr u64 kSectorSize = 512;     // the same: the sector size of every device that can have free space
constexpr u32 kMaxMountPointLength = 31; // CELL_FS_MAX_MP_LENGTH

struct GuestPath {
    std::string text;    // up to the NUL, 1025 bytes at most
    bool faulted = false; // ran into memory that is not guest memory first
};

GuestPath read_path(lv2::PpuThread& t, u32 addr)
{
    GuestPath p;
    for (u32 i = 0; i <= 1024; i++) {
        if (!guest_readable(t, addr + i, 1)) {
            p.faulted = true;
            break;
        }
        const char c = static_cast<char>(ps_read8(t.kernel.base(), addr + i));
        if (!c) break;
        p.text += c;
    }
    return p;
}

// RPCS3 sys_fs_disk_free's free bytes for `vpath` (cellFsGetFreeSize's use of it, the paths not under /dev_hdd0): an
// empty path, one not starting with exactly one '/', or a device that isn't mounted is CELL_EINVAL; "/", /app_home and
// /host_root (RPCS3's strict_get_block_size devices) CELL_ENOTSUP; a path of more than 31 characters
// CELL_ENAMETOOLONG. A read-only device has none free: /dev_bdvd and /dev_ps2disc (read-only mount points), /dev_flash
// (RPCS3 mounts it read-only) and whatever the host mounted read-only (lv2's stat then reports no write permission).
// /dev_hdd1 has 2 GB less a sector free, any other device 40 GB less a sector (RPCS3's sizes, see cellHddGameCheck).
s32 disk_free(PPUContext& ctx, const std::string& vpath, u64& available)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    available = 0;
    if (vpath.empty()) return error(lv2::CELL_EINVAL);
    if (vpath == "/") return error(lv2::CELL_ENOTSUP);
    if (vpath.size() > kMaxMountPointLength) return error(lv2::CELL_ENAMETOOLONG);
    if (vpath.find_first_not_of('/') != 1) return error(lv2::CELL_EINVAL);
    const std::string device = vpath.substr(0, vpath.find('/', 1));
    StackVar v(ctx, base, 0x20 + lv2::kCellFsStatSize); // the device's path (StackVar clears: terminated), CellFsStat
    std::memcpy(base + v.addr(), device.data(), device.size());
    if (lv2::sys_fs_stat(t, v.addr(), v.addr() + 0x20) != lv2::CELL_OK) return error(lv2::CELL_EINVAL);
    if (device == "/app_home" || device == "/host_root") return error(lv2::CELL_ENOTSUP);
    const bool read_only = device == "/dev_bdvd" || device == "/dev_ps2disc" || device == "/dev_flash" ||
        !(ps_read32(base, v.addr() + 0x20) & 0222);
    if (!read_only) available = device == "/dev_hdd1" ? 2 * kGiB - kSectorSize : 40 * kGiB - kSectorSize;
    return lv2::CELL_OK;
}

// cellFsGetFreeSize(path, u32* block_size, u64* block_count): RPCS3's. CELL_EFAULT for a null argument; both results
// are zeroed first. A path starting "/dev_hdd0" gets RPCS3's sys_fs_fcntl 0xC0000002 answer, whether the path exists
// or not: 40 GB less a byte in /dev_hdd0's 4 KB blocks. Any other path takes disk_free's free bytes (no free bytes:
// CELL_ENOTDIR), then the path's block size (cellFsGetBlockSize: the path must exist, lv2's stat errors otherwise),
// and the block count is the free bytes over it. Deviation (D-042): a path or result pointer outside guest memory is
// CELL_EFAULT with the call site logged (RPCS3 faults).
s32 cellFsGetFreeSize(PPUContext& ctx, u32 path, u32 block_size, u32 block_count)
{
    if (!path || !block_size || !block_count) return error(lv2::CELL_EFAULT);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const GuestPath p = read_path(t, path);
    // RPCS3 compares only the first 9 bytes with "/dev_hdd0".
    const bool hdd0 = p.text.starts_with("/dev_hdd0");
    if ((p.faulted && !hdd0) || !guest_readable(t, block_size, 4) || !guest_readable(t, block_count, 8)) {
        log_call_site(ctx, base,
            std::format("cellFsGetFreeSize(path {:#x}, block_size {:#x}, block_count {:#x}): not guest memory", path,
                block_size, block_count));
        return error(lv2::CELL_EFAULT);
    }
    ps_write32(base, block_size, 0);
    ps_write64(base, block_count, 0);
    if (hdd0) {
        ps_write64(base, block_count, (40 * kGiB - 1) / kHdd0BlockSize);
        ps_write32(base, block_size, kHdd0BlockSize);
        PS_LOG_INFO(kChan, "cellFsGetFreeSize({}): {} blocks of {} bytes", p.text, ps_read64(base, block_count),
            kHdd0BlockSize);
        return lv2::CELL_OK;
    }
    u64 available = 0;
    s32 r = disk_free(ctx, p.text, available);
    // (RPCS3 turns sys_fs_disk_free's CELL_EPERM into CELL_EINVAL; that one never returns it.)
    if (r == lv2::CELL_OK && !available) r = error(lv2::CELL_ENOTDIR);
    if (r == lv2::CELL_OK) {
        StackVar sb(ctx, base, lv2::kCellFsStatSize);
        r = lv2::sys_fs_stat(t, path, sb.addr());
        if (r == lv2::CELL_OK) {
            const u64 block = ps_read64(base, sb.addr() + 44); // CellFsStat st_blksize: the device's block size
            ps_write64(base, block_count, available / block);
            ps_write32(base, block_size, static_cast<u32>(block));
        }
    }
    if (r != lv2::CELL_OK) {
        PS_LOG_WARN(kChan, "cellFsGetFreeSize({}) -> {} ({:#010x})", p.text, lv2::error_name(r), static_cast<u32>(r));
        return r;
    }
    PS_LOG_INFO(kChan, "cellFsGetFreeSize({}): {} blocks of {} bytes", p.text, ps_read64(base, block_count),
        ps_read32(base, block_size));
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------- asynchronous reads (AIO)

constexpr u32 NID_aio_thread = 0xFFB98633; // nid_of("_pspocket_fs_aio_thread"): pspocket's own
constexpr s32 kAioThreadPriority = 1000;
constexpr u32 kAioThreadStack = 0x10000;

// CellFsAio (40 bytes): be32 fd; be64 offset at 8; be32 buf at 16; be64 size at 24; be64 user_data at 32.
namespace aio_off {
constexpr u32 fd = 0, offset = 8, buf = 16, size = 24, kSize = 40;
}

struct AioRequest {
    s32 xid = 0;
    u32 aio = 0;  // the game's CellFsAio
    u32 func = 0; // the descriptor of its callback(aio, error, xid, size)
    bool write = false; // cellFsAioWrite
};

// RPCS3 fs_aio_manager: the AIO thread, the requests waiting for it, and the last request ID (RPCS3 g_fs_aio_id).
struct AioState {
    std::mutex mutex;
    u32 thread_id = 0; // 0 until the first cellFsAioInit
    std::deque<AioRequest> requests;
    lv2::WaitQueue idle; // the AIO thread, while it waits for a request
    s32 last_id = 0;
    u32 reads = 0; // requests so far, for the log
};

AioState& aio_state()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "cellFsAio called without an HLE process");
    return p->state<AioState>();
}

// The AIO thread's body (RPCS3 fs_aio_thread::non_task), its entry through an internal function's descriptor. It takes
// the requests in order. Each reads aio->size bytes at aio->offset into aio->buf without moving the file position (as
// cellFsReadWithOffset: sys_fs_fcntl 0x8000000A), then the game's func(aio, error, xid, size read) runs on this
// thread. A descriptor that isn't a file open for reading gives CELL_EBADF and 0 bytes (RPCS3). Deviations: a buffer
// outside guest memory gives CELL_EFAULT, and a host read error that error with the bytes read before it (RPCS3
// faults, or aborts). Never returns: the thread unwinds when the process ends.
void _pspocket_fs_aio_thread(PPUContext& ctx, u64)
{
    AioState& s = aio_state();
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    for (;;) {
        AioRequest req;
        {
            std::unique_lock lk(s.mutex);
            while (s.requests.empty()) {
                lv2::prepare_wait(t.waiter);
                s.idle.push(&t.waiter);
                const lv2::WaitStatus status = lv2::park(t.waiter, lk, 0, {lv2::WaitKind::Sleep, 0}, [&](bool) {
                    return s.idle.remove(&t.waiter) ? lv2::CancelResult::Removed : lv2::CancelResult::Woken;
                });
                if (status == lv2::WaitStatus::Aborted) throw lv2::ThreadExit{};
                lk.lock();
            }
            req = s.requests.front();
            s.requests.pop_front();
        }
        s32 result = error(lv2::CELL_EFAULT);
        u64 done = 0;
        if (guest_readable(t, req.aio, aio_off::kSize)) {
            StackVar count(ctx, base, 8);
            result = rw_with_offset(ctx, req.write ? 0x8000000B : 0x8000000A, req.write ? 0xFA8B0000 : 0xFA8A0000,
                ps_read32(base, req.aio + aio_off::fd),
                ps_read64(base, req.aio + aio_off::offset), ps_read32(base, req.aio + aio_off::buf),
                ps_read64(base, req.aio + aio_off::size), count.addr());
            done = ps_read64(base, count.addr());
        } else {
            PS_LOG_ERROR(kChan, "AIO request {}: its CellFsAio at {:#x} is no longer guest memory", req.xid, req.aio);
        }
        PS_LOG_DEBUG(kChan, "AIO request {} done: {}, {:#x} bytes; calling {:#x}", req.xid, lv2::error_name(result),
            done, req.func);
        call_guest(ctx, req.func, {req.aio, static_cast<u64>(static_cast<s64>(result)),
                                      static_cast<u64>(static_cast<s64>(req.xid)), done}); // returns void
    }
}

// Creates and starts the AIO thread: an ordinary priority, a 64 KB stack and a TLS block like any thread's (RPCS3,
// whose HLE never creates it, gives no parameters). Called with the AIO state locked.
s32 start_aio_thread(PPUContext& ctx, AioState& s)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    HleProcess& p = *HleProcess::current();
    const u32 entry = p.descriptor_of(kModule, NID_aio_thread);
    if (!entry) PS_FATAL(kChan, "the AIO thread function was not linked at load");
    const u32 tls = p.alloc_tls();
    if (!tls) return error(lv2::CELL_ENOMEM);
    StackVar v(ctx, base, 0x30); // thread ID (u64), ppu_thread_param_t {entry, tls}, the name
    ps_write32(base, v.addr() + 8, entry);
    ps_write32(base, v.addr() + 12, tls + kTlsR13Offset);
    std::memcpy(base + v.addr() + 0x10, "fs_aio_thread", 14);
    if (const s32 r = lv2::_sys_ppu_thread_create(t, v.addr(), v.addr() + 8, 0, 0, kAioThreadPriority,
            kAioThreadStack, 0, v.addr() + 0x10);
        r != lv2::CELL_OK) {
        p.free_tls(tls);
        return r;
    }
    const u32 id = static_cast<u32>(ps_read64(base, v.addr()));
    if (const s32 r = lv2::sys_ppu_thread_start(t, id); r != lv2::CELL_OK) return r;
    s.thread_id = id;
    return lv2::CELL_OK;
}

// cellFsAioInit(mount_point): the first call starts the AIO thread, which then serves every mount point (RPCS3's
// fs_aio_manager holds one thread; its TODO is one per mount point). The mount point is not checked (RPCS3). CELL_OK,
// or the error creating the thread gave.
s32 cellFsAioInit(PPUContext& ctx, u32 mount_point)
{
    AioState& s = aio_state();
    std::lock_guard lock(s.mutex);
    if (s.thread_id) {
        PS_LOG_INFO(kChan, "cellFsAioInit({}): the AIO thread {:#x} runs already", path_of(ctx, mount_point),
            s.thread_id);
        return lv2::CELL_OK;
    }
    if (const s32 r = start_aio_thread(ctx, s); r != lv2::CELL_OK) {
        PS_LOG_ERROR(kChan, "cellFsAioInit({}): the AIO thread was not created: {}", path_of(ctx, mount_point),
            lv2::error_name(r));
        return r;
    }
    PS_LOG_INFO(kChan, "cellFsAioInit({}): AIO thread {:#x} started", path_of(ctx, mount_point), s.thread_id);
    return lv2::CELL_OK;
}

// cellFsAioFinish(mount_point): CELL_OK (RPCS3). Its TODO, ending the mount point's AIO thread, isn't done there
// either: the thread stays, and the requests queued before still complete.
s32 cellFsAioFinish(PPUContext& ctx, u32 mount_point)
{
    PS_LOG_INFO(kChan, "cellFsAioFinish({})", path_of(ctx, mount_point));
    return lv2::CELL_OK;
}

// cellFsAioRead / cellFsAioWrite(CellFsAio* aio, s32* id, func): RPCS3's. CELL_ENXIO until cellFsAioInit started the
// AIO thread; otherwise the request gets the next ID (from 1), written to *id, and is queued for the AIO thread, and
// the call returns at once: the callback brings the result. Deviation (D-042): a null aio, id or func, or one outside
// guest memory, is CELL_EFAULT with the call site logged (RPCS3 faults, at once or on the AIO thread).
s32 aio_submit(PPUContext& ctx, u32 aio, u32 id, u32 func, bool write)
{
    const char* fn = write ? "cellFsAioWrite" : "cellFsAioRead";
    lv2::PpuThread& t = thread_of(ctx);
    AioState& s = aio_state();
    std::lock_guard lock(s.mutex);
    if (!s.thread_id) {
        PS_LOG_WARN(kChan, "{}(aio {:#x}): cellFsAioInit has not started the AIO thread: CELL_ENXIO", fn, aio);
        return error(lv2::CELL_ENXIO);
    }
    if (!aio || !id || !func || !guest_readable(t, aio, aio_off::kSize) || !guest_readable(t, id, 4) ||
        !guest_readable(t, func, 8)) {
        log_call_site(ctx, guest_base(ctx),
            std::format("{}(aio {:#x}, id {:#x}, func {:#x}): a null pointer, or one outside guest memory", fn,
                aio, id, func));
        return error(lv2::CELL_EFAULT);
    }
    u8* base = t.kernel.base();
    const s32 xid = s.last_id = static_cast<s32>(static_cast<u32>(s.last_id) + 1);
    ps_write32(base, id, static_cast<u32>(xid));
    s.requests.push_back({xid, aio, func, write});
    for (lv2::Waiter* w : s.idle.take_all(lv2::SYS_SYNC_FIFO)) lv2::wake(*w, lv2::CELL_OK);
    const log::Level level = s.reads++ < 8 ? log::Level::Info : log::Level::Debug;
    if (level >= log::min_level()) {
        log::print(level, kChan, "{}: request {}: fd {}, {:#x} bytes at {:#x}, buffer {:#x}, callback {:#x}", fn, xid,
            ps_read32(base, aio + aio_off::fd), ps_read64(base, aio + aio_off::size),
            ps_read64(base, aio + aio_off::offset), ps_read32(base, aio + aio_off::buf), func);
    }
    return lv2::CELL_OK;
}

s32 cellFsAioRead(PPUContext& ctx, u32 aio, u32 id, u32 func) { return aio_submit(ctx, aio, id, func, false); }
s32 cellFsAioWrite(PPUContext& ctx, u32 aio, u32 id, u32 func) { return aio_submit(ctx, aio, id, func, true); }

// cellFsAioCancel(id): CELL_EINVAL, as in RPCS3: requests aren't cancelled, each one completes with its callback.
// (RPCS3's TODO: a cancelled request's callback gets CELL_ECANCELED.)
s32 cellFsAioCancel(PPUContext&, s32 id)
{
    PS_LOG_WARN(kChan, "cellFsAioCancel({}): requests are not cancelled (RPCS3): CELL_EINVAL", id);
    return error(lv2::CELL_EINVAL);
}

// An NPD file's plain bytes, as the kernel reads an image file (D-165).
class NpdPlainFile final : public lv2::VfsImage::File {
public:
    explicit NpdPlainFile(std::unique_ptr<recomp::NpdFile> npd) : npd_(std::move(npd)) {}
    u64 size() const override { return npd_->size(); }
    bool read(u64 offset, void* dst, usize size) const override
    {
        std::string error;
        if (npd_->read(offset, dst, size, &error)) return true;
        PS_LOG_ERROR(kChan, "encrypted file ({}): {}", npd_->header().content_id, error);
        return false;
    }

private:
    std::unique_ptr<recomp::NpdFile> npd_;
};

} // namespace

void install_npd_opener(lv2::Kernel& kernel)
{
    kernel.set_npd_opener([](std::shared_ptr<const lv2::VfsImage::File> encrypted, bool edata,
                              std::string& error) -> std::shared_ptr<const lv2::VfsImage::File> {
        const u64 size = encrypted->size();
        auto npd = recomp::NpdFile::open(
            [encrypted](u64 offset, void* dst, usize n) { return encrypted->read(offset, dst, n); }, size,
            recomp::builtin_npd_keys());
        if (!npd) {
            error = npd.error().message;
            return nullptr;
        }
        if (edata != !((*npd)->header().flags & recomp::kNpdSdatFlag)) {
            PS_LOG_WARN(kChan, "an {} file opened as {}", edata ? "SDATA" : "EDATA", edata ? "EDATA" : "SDATA");
        }
        PS_LOG_INFO(kChan, "encrypted file: NPD version {}, flags {:#x}, {:#x}-byte blocks, {} bytes", (*npd)->header().version,
            (*npd)->header().flags, (*npd)->header().block_size, (*npd)->size());
        return std::make_shared<NpdPlainFile>(std::move(*npd));
    });
}

void register_sys_fs(Registry& r)
{
    r.add("sys_fs", {
        PS_HLE_FUNC(0x718BF5F8, cellFsOpen),
        PS_HLE_FUNC(0xB1840B53, cellFsSdataOpen),
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
        PS_HLE_FUNC(0xEF3EFA34, cellFsFstat),
        PS_HLE_FUNC(0xAA3B4BCD, cellFsGetFreeSize),
        PS_HLE_FUNC(0xD73938DF, cellFsStReadFinish),
        PS_HLE_FUNC(0xDB869F20, cellFsAioInit),
        PS_HLE_FUNC(0x9F951810, cellFsAioFinish),
        PS_HLE_FUNC(0xC1C507E7, cellFsAioRead),
        PS_HLE_FUNC(0x7F13FC8C, cellFsAioCancel),
        PS_HLE_INTERNAL(NID_aio_thread, _pspocket_fs_aio_thread),
        PS_HLE_FUNC(0xC9DC3AC5, cellFsTruncate),
        PS_HLE_FUNC(0xD338A65A, cellFsFdatasync),
        PS_HLE_FUNC(0x1A108AB7, cellFsGetBlockSize),
        PS_HLE_FUNC(0xCB588DBA, cellFsFGetBlockSize),
        PS_HLE_FUNC(0x91D59F69, cellFsGetPath),
        PS_HLE_FUNC(0x9B882495, cellFsGetDirectoryEntries),
        PS_HLE_FUNC(0x75F16DC5, cellFsSetIoBufferFromDefaultContainer),
        PS_HLE_FUNC(0x2664C8AE, cellFsStReadInit),
        PS_HLE_FUNC(0xB3AFEE8B, cellFsStReadGetRingBuf),
        PS_HLE_FUNC(0xCF34969C, cellFsStReadGetStatus),
        PS_HLE_FUNC(0xBD273A88, cellFsStReadGetRegid),
        PS_HLE_FUNC(0x8DF28FF9, cellFsStReadStart),
        PS_HLE_FUNC(0xF8E5D9A0, cellFsStReadStop),
        PS_HLE_FUNC(0x27800C6B, cellFsStRead),
        PS_HLE_FUNC(0x190912F6, cellFsStReadGetCurrentAddr),
        PS_HLE_FUNC(0x81F33783, cellFsStReadPutCurrentAddr),
        PS_HLE_FUNC(0x8F71C5B2, cellFsStReadWait),
        PS_HLE_FUNC(0x866F6AEC, cellFsStReadWaitCallback),
        PS_HLE_FUNC(0x4CEF342E, cellFsAioWrite),
        PS_HLE_FUNC(0x6D3BB15B, cellFsSdataOpenByFd),
    });
}

} // namespace pspocket::hle
