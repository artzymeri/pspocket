// fs_utility_init: the system software's disk utilities, from libfs_utility_init.sprx (4.92, /dev_flash/sys/internal;
// D-935), compared with it call for call (tests/hle/test_fs_utility_init.cpp). RPCS3 has them as placeholders: the
// system menu checks, repairs and grows its hard disk's file systems with them as it starts; a game has no use for
// them. Here each does what the module does on pspocket's lv2, in a game's process:
// - 665DF255 / E7563CE6 (char* out): the hard disk's serial number (20 characters) and model (40), copied out of an ATA
//   IDENTIFY DEVICE that sys_storage runs on the disk (sys_storage_open is a root process's call: a game's is refused,
//   CELL_ENOSYS, the answer, and `out` is left as it was). Null: CELL_EFAULT.
// - 6B5896B0 (u64* count): the disk's partitions (RPCS3: 2), from the module's disk object, which asks lv2 for the
//   disk's information (sys_storage_get_device_info, a root process's call) up to three times; without it the count is
//   0 and the answer CELL_ENODEV.
// - 4E949DA4 (): mounts the disk's raw device ("CELL_FS_UTILITY:HDD0", CELL_FS_SIMPLEFS) at /dev_simple_hdd0, reads the
//   word at 0x10520 and writes it back, closes and unmounts. A game's mount is refused (CELL_EPERM): the mount point is
//   unmounted all the same and the mount's error is the answer.
// - A9B04535 / 263172B8 / 1F3CD9F1 (argc, argv[, memory, size[, callback, notification ID]]): FreeBSD 7.2's fsck_ffs
//   (sbin/fsck_ffs, in external/freebsd-7.2) as Sony built it for lv2: its own getopt, exit() a longjmp back to the
//   function (whose answer is the exit status), file I/O through lv2's file system, the disk itself through a mount of
//   its raw device ("TemporalMount") when the path is hdd0, /dev_hdd0 or CELL_FS_UTILITY:HDD0. Its memory is a heap
//   of its own: liblv2's, of 2 MB with -F (32 MB without) from lv2 for A9B04535, in the caller's memory for the others
//   (which must be that large: CELL_ENOMEM). 1F3CD9F1 also keeps a callback and notification ID for the run.
// - F691D443 (argc, argv, memory, size): FreeBSD's growfs, which mounts the raw device at /dev_hdd0 first; a game's
//   mount is refused and it answers CELL_EIO.
// Ported is what a game's process reaches on the disk and on names that do not exist: every option, the heaps, the
// exits, the messages (printed through liblv2's printf, as the module prints them) and every lv2 call. The disk itself
// is never a game's (its mount is refused, and pspocket's lv2 runs no root process), so checking and growing a file
// system are not ported; nor is a check of a file or directory that exists (the module mounts nothing for one and reads
// it block by block as a disk, to answer 8 nearly always): that logs and exits 8. The module's strtol (for -b, -c and
// -m) is a stub that answers 0 and leaves the end pointer unset, which the module then reads (address 0 on a clean
// stack: it faults); here the 0 is taken.
//
// The module keeps its state in globals and takes no lock; here one call runs at a time.

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fs_utility_init.hpp"
#include "guest.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "sysutil_sub.hpp"

namespace pspocket::hle {
namespace {

using sysutil::sub::call_sysutil;
using sysutil::sub::module_printf;

constexpr std::string_view kChan = "fs_utility_init";

// lv2's errors, as the module has them.
constexpr u32 kEINVAL = 0x80010002, kENOSYS = 0x80010003, kENOMEM = 0x80010004,
              kENOENT = 0x80010006, kEBUSY = 0x8001000A, kEFAULT = 0x8001000D, kEISDIR = 0x80010012,
              kEBADF = 0x8001002A, kEIO = 0x8001002B, kENODEV = 0x8001002D;

constexpr u64 kAtaHdd = 0x101000000000007; // the hard disk's sys_storage device
constexpr u32 kDeviceFlagsInit = 0x1000000; // the device object's third word as fn_e548 starts it

// lv2 calls the module makes with sc.
enum : u32 {
    SC_STORAGE_OPEN = 600, SC_STORAGE_CLOSE = 601, SC_STORAGE_GET_DEVICE_INFO = 609,
    SC_STORAGE_EXECUTE_DEVICE_COMMAND = 616, SC_FS_OPEN = 801, SC_FS_READ = 802, SC_FS_WRITE = 803, SC_FS_CLOSE = 804,
    SC_FS_OPENDIR = 805, SC_FS_CLOSEDIR = 807, SC_FS_STAT = 808, SC_FS_LSEEK = 818, SC_FS_MOUNT = 837,
    SC_FS_UNMOUNT = 838,
};

u32 sc(PPUContext& ctx, u32 n, std::initializer_list<u64> args)
{
    return static_cast<u32>(lv2_syscall(ctx, n, args));
}

// A string of the module's on the stack, for an lv2 call or printf.
class GuestText {
public:
    GuestText(PPUContext& ctx, std::string_view text)
        : var_(ctx, guest_base(ctx), static_cast<u32>(text.size()) + 1)
    {
        std::memcpy(guest_base(ctx) + var_.addr(), text.data(), text.size());
    }
    u32 addr() const { return var_.addr(); }

private:
    StackVar var_;
};

// The module's state that outlives a call.
struct Module {
    std::mutex call;
    // fsck's getopt: its argument and the program's name stay from call to call (its index does not, below).
    u32 optarg = 0, progname = 0;
    s32 lfmode = 0700; // -m: not among the globals a run resets
    // 1F3CD9F1's callback and notification ID.
    u32 callback = 0;
    u64 notification = 0;
    // The disk object (6B5896B0): guest memory as the module lays it out (its device at +8, the device's information
    // at +0x18, the flags at +0x58: 1 opened, 2 information read), made on the first call.
    u32 disk = 0;
    // The identify buffer (0x200 bytes).
    u32 identify = 0;
};

Module& module() { return HleProcess::current()->state<Module>(); }

constexpr u32 kDiskSize = kFsUtilityInitDiskSize, kDiskInfo = 0x18, kDiskFlags = 0x58;

// A block of `size` bytes from liblv2's _sys_malloc (as the module's operator new), zeroed.
u32 malloc_block(PPUContext& ctx, u32 size)
{
    const u32 p = static_cast<u32>(call_sysutil(ctx, "sysPrxForUser", "_sys_malloc", {size}));
    if (p) std::memset(guest_base(ctx) + p, 0, size);
    return p;
}

u32 disk_object(PPUContext& ctx, Module& m)
{
    if (!m.disk) {
        m.disk = malloc_block(ctx, kDiskSize);
        if (m.disk) {
            ps_write64(guest_base(ctx), m.disk + 8, kAtaHdd);
            ps_write32(guest_base(ctx), m.disk + kDiskFlags, 1);
        }
    }
    return m.disk;
}

// ------------------------------------------------------------------------------------------------- the disk

// fn_2dd0: an ATA IDENTIFY DEVICE (0xEC) into `buf` (0x200 bytes, cleared first) through sys_storage.
u32 identify_device(PPUContext& ctx, u32 buf)
{
    u8* b = guest_base(ctx);
    std::memset(b + buf, 0, 0x200);
    StackVar cmd(ctx, b, 0x20);
    ps_write8(b, cmd.addr() + 0x0B, 0xEC);
    ps_write32(b, cmd.addr() + 0x0C, 1);
    ps_write32(b, cmd.addr() + 0x10, 1);
    ps_write32(b, cmd.addr() + 0x14, 1);
    ps_write32(b, cmd.addr() + 0x18, 0x200);
    StackVar fd(ctx, b, 8), status(ctx, b, 8);
    u32 r = sc(ctx, SC_STORAGE_OPEN, {kAtaHdd, 0, fd.addr(), 0});
    if (r != 0) return r;
    r = sc(ctx, SC_STORAGE_EXECUTE_DEVICE_COMMAND, {ps_read32(b, fd.addr()), 2, cmd.addr(), 0x20, buf, 0x200,
        status.addr()});
    if (r == 0) {
        if (static_cast<s8>(ps_read8(b, buf + 1)) < 0) module_printf(ctx, "%s: not ATA device\n", {"identify_device"});
        if (ps_read8(b, buf) & 4) module_printf(ctx, "%s: identify device incomplete\n", {"identify_device"});
    } else {
        module_printf(ctx, "%s: sys_storage_execute_device_command failed[%#x]\n", {"identify_device", r});
    }
    sc(ctx, SC_STORAGE_CLOSE, {ps_read32(b, fd.addr())});
    return r;
}

// 665DF255 and E7563CE6: the identify data's words [first, end) to `out`, each word's two bytes swapped, then a NUL.
s32 identify_string(PPUContext& ctx, u32 out, u32 first, u32 end)
{
    if (!out) return static_cast<s32>(kEFAULT);
    Module& m = module();
    std::lock_guard lock(m.call);
    if (!m.identify) m.identify = malloc_block(ctx, 0x200);
    if (!m.identify) return static_cast<s32>(kENOMEM);
    const u32 r = identify_device(ctx, m.identify);
    if (r != 0) return static_cast<s32>(r);
    if (!guest_readable(thread_of(ctx), out, end - first + 1)) {
        log_call_site(ctx, guest_base(ctx), "fs_utility_init: the output is not guest memory (the module faults)");
        return static_cast<s32>(kEFAULT);
    }
    u8* b = guest_base(ctx);
    u32 o = out;
    for (u32 at = m.identify + first; at < m.identify + end; at += 2) {
        ps_write8(b, o, ps_read8(b, at + 1));
        ps_write8(b, o + 1, ps_read8(b, at));
        o += 2;
    }
    ps_write8(b, o, 0);
    return 0;
}

s32 fs_utility_init_665DF255(PPUContext& ctx, u32 serial) { return identify_string(ctx, serial, 0x14, 0x28); }
s32 fs_utility_init_E7563CE6(PPUContext& ctx, u32 model) { return identify_string(ctx, model, 0x36, 0x5E); }

// fn_1194: the disk's information, when the object is open: 0, or CELL_EIO when lv2 has none (the module also asks
// whether the disk has 4 KB sectors once lv2 answers, which only a root process's lv2 does).
u32 disk_query(PPUContext& ctx, u32 disk)
{
    u8* b = guest_base(ctx);
    const u32 flags = ps_read32(b, disk + kDiskFlags);
    if (!(flags & 1)) return kEINVAL;
    ps_write32(b, disk + kDiskFlags, flags & ~2u);
    if (sc(ctx, SC_STORAGE_GET_DEVICE_INFO, {ps_read64(b, disk + 8), disk + kDiskInfo}) != 0) return kEIO;
    PS_LOG_ERROR(kChan, "fs_utility_init: lv2 has the hard disk's information (a root process): its sector size and "
                        "partitions are not supported");
    return kEIO;
}

// 6B5896B0 (fn_1938): the partitions. The information is asked for three times at most; without a capacity, none.
s32 fs_utility_init_6B5896B0(PPUContext& ctx, u32 count)
{
    if (!count) return static_cast<s32>(kEFAULT);
    Module& m = module();
    std::lock_guard lock(m.call);
    const u32 disk = disk_object(ctx, m);
    if (!disk) return static_cast<s32>(kENOMEM);
    u8* b = guest_base(ctx);
    const auto valid = [&] { return (ps_read32(b, disk + kDiskFlags) & 2) != 0; };
    if (!valid()) {
        disk_query(ctx, disk);
        for (int tries = 2; !valid() && tries != 0; tries--) disk_query(ctx, disk);
    }
    if (!guest_readable(thread_of(ctx), count, 8)) {
        log_call_site(ctx, b, "fs_utility_init_6B5896B0: the count is not guest memory (the module faults)");
        return static_cast<s32>(kEFAULT);
    }
    // Information read is a root process's: not supported (disk_query logged it), so no capacity here.
    ps_write64(b, count, 0);
    return static_cast<s32>(kENODEV);
}

// fn_32e4: sys_fs_mount of `device` (`fs`) at `path`, with the option list the module never fills (empty here).
u32 mount(PPUContext& ctx, std::string_view device, std::string_view fs, std::string_view path, u32 prot = 0)
{
    GuestText d(ctx, device), f(ctx, fs), p(ctx, path);
    StackVar options(ctx, guest_base(ctx), 0x30);
    return sc(ctx, SC_FS_MOUNT, {d.addr(), f.addr(), p.addr(), 0, prot, 0, options.addr(), 0});
}

u32 unmount(PPUContext& ctx, std::string_view path, u32 force = 0)
{
    GuestText p(ctx, path);
    return sc(ctx, SC_FS_UNMOUNT, {p.addr(), 0, force});
}

// 4E949DA4: the word at 0x10520 of the raw device, read and written back.
s32 fs_utility_init_4E949DA4(PPUContext& ctx)
{
    Module& m = module();
    std::lock_guard lock(m.call);
    constexpr std::string_view kPoint = "/dev_simple_hdd0";
    u32 r = mount(ctx, "CELL_FS_UTILITY:HDD0", "CELL_FS_SIMPLEFS", kPoint);
    if (r == 0) {
        u8* b = guest_base(ctx);
        GuestText p(ctx, kPoint);
        StackVar fd(ctx, b, 8), pos(ctx, b, 8), word(ctx, b, 8), done(ctx, b, 8);
        r = sc(ctx, SC_FS_OPEN, {p.addr(), 2, fd.addr(), 0, 0, 0});
        if (r == 0) {
            const u32 f = ps_read32(b, fd.addr());
            r = sc(ctx, SC_FS_LSEEK, {f, 0x10520, 0, pos.addr()});
            if (r == 0) r = sc(ctx, SC_FS_READ, {f, word.addr(), 4, done.addr()});
            if (r == 0) r = sc(ctx, SC_FS_LSEEK, {f, 0x10520, 0, pos.addr()});
            if (r == 0) r = sc(ctx, SC_FS_WRITE, {f, word.addr(), 4, done.addr()});
            const u32 c = sc(ctx, SC_FS_CLOSE, {f});
            if (r == 0) r = c;
            if (r == 0) r = unmount(ctx, kPoint);
            if (r == 0) return 0;
        }
    }
    unmount(ctx, kPoint);
    return static_cast<s32>(r);
}

// ------------------------------------------------------------------------------------------------- growfs

// F691D443 (fn_be38, fn_a494): growfs mounts the raw device at /dev_hdd0 (fn_bc1c) and opens it twice before anything
// else; a refused mount prints its error and the answer is CELL_EIO.
s32 fs_utility_init_F691D443(PPUContext& ctx, u32, u32, u32, u32)
{
    Module& m = module();
    std::lock_guard lock(m.call);
    const u32 r = mount(ctx, "CELL_FS_UTILITY:HDD0", "CELL_FS_SIMPLEFS", "/dev_hdd0");
    if (static_cast<s32>(r) < 0) {
        module_printf(ctx, "Error Status: %d\n", {1u});
        module_printf(ctx, "Mount failed 0x%x\n", {r});
        module_printf(ctx, "\n", {});
        return static_cast<s32>(kEIO);
    }
    // The mount of the disk is not a game's (refused) and pspocket's lv2 has none for a root process: growing a file
    // system is not supported.
    PS_LOG_ERROR(kChan, "fs_utility_init_F691D443: the hard disk mounted: growing its file system is not supported");
    unmount(ctx, "/dev_hdd0");
    return static_cast<s32>(kEIO);
}

// ------------------------------------------------------------------------------------------------- fsck

struct Exit {
    s32 status;
};

// One run of fsck: fsck_ffs's globals, which each run starts afresh (fn_e574, fn_e818), and its heap.
struct Fsck {
    PPUContext& ctx;
    Module& m;
    u8* b;
    u32 heap = 0;
    u32 device = 0; // the device object {fd, raw fd, flags} (fn_e944), in the heap
    u32 err = 0;    // the module's errno
    bool exit_zero = false; // fn_ee98's flag: the exit was exit(0)
    s32 optind = 0;
    // main.c's flags.
    u8 nflag = 0, yflag = 0, preen = 0, skipclean = 0;
    s32 bflag = 0, bkgrdflag = 0, cvtlevel = 0, debug = 0, bkgrdcheck = 0;
    u32 cdevname = 0;
    s32 fsreadfd = -1;
    u32 sblk = 0, asblk = 0; // the superblock buffers

    Fsck(PPUContext& c, Module& mod) : ctx(c), m(mod), b(guest_base(c)) {}

    [[noreturn]] void exit(s32 status)
    {
        exit_zero = status == 0;
        throw Exit{status};
    }

    std::string text(u32 addr) const { return read_cstr(thread_of(ctx), addr, 0x400).text; }

    // ---- the heap (liblv2's)

    u32 heap_malloc(u32 size)
    {
        return static_cast<u32>(call_sysutil(ctx, "sysPrxForUser", "_sys_heap_malloc", {heap, size}));
    }
    void heap_free(u32 p) { call_sysutil(ctx, "sysPrxForUser", "_sys_heap_free", {heap, p}); }

    // fn_ea84: a heap of `size` from lv2.
    bool create_heap(u32 size)
    {
        GuestText name(ctx, "cellFsck_Library");
        heap = static_cast<u32>(call_sysutil(ctx, "sysPrxForUser", "_sys_heap_create_heap",
            {name.addr(), size, 0x150000, 0}));
        if (heap) return true;
        module_printf(ctx, "fsck %s: initialization failed\n", {"my_sys_create_heap"});
        return false;
    }
    // fn_e9fc: a heap in the caller's memory.
    bool create_heap_at(u32 memory, u32 size)
    {
        GuestText name(ctx, "cellFsck_Library");
        StackVar attr(ctx, b, 8);
        ps_write32(b, attr.addr(), 8);
        ps_write32(b, attr.addr() + 4, memory);
        heap = static_cast<u32>(call_sysutil(ctx, "sysPrxForUser", "_sys_heap_create_heap",
            {name.addr(), size, 0x80150000, attr.addr()}));
        if (heap) return true;
        module_printf(ctx, "fsck %s: initialization failed\n", {"my_sys_create_heap2"});
        return false;
    }
    void delete_heap()
    {
        if (heap) call_sysutil(ctx, "sysPrxForUser", "_sys_heap_delete_heap", {heap});
        heap = 0;
    }

    // fn_e944: the device object.
    bool create_device()
    {
        device = heap_malloc(0xC);
        if (!device) {
            heap_free(0);
            return false;
        }
        ps_write32(b, device, 0xFFFFFFFF);
        ps_write32(b, device + 4, 0xFFFFFFFF);
        ps_write32(b, device + 8, kDeviceFlagsInit);
        return true;
    }

    // fn_fa38: the run's end: the device object's descriptors closed (fn_f964), the object freed, the heap deleted.
    void finish()
    {
        if (device) {
            if (raw() != -1) device_close(raw());
            if (mounted() != -1) device_close(mounted());
            heap_free(device);
            device = 0;
        }
        delete_heap();
    }

    // The device object's words: the descriptor whose close unmounts the raw device, the raw device's descriptor.
    s32 mounted() const { return static_cast<s32>(ps_read32(b, device)); }
    s32 raw() const { return static_cast<s32>(ps_read32(b, device + 4)); }

    // lv2's close of `fd`, and closedir when lv2 says it is a directory or no file (fn_eff0, fn_10d28).
    u32 close_fd(s32 fd)
    {
        const u64 arg = static_cast<u64>(static_cast<s64>(fd)); // the module's int, sign-extended in its register
        u32 r = sc(ctx, SC_FS_CLOSE, {arg});
        if (r == kEISDIR || r == kEBADF) r = sc(ctx, SC_FS_CLOSEDIR, {arg});
        return r;
    }

    // simple_umount (fn_11b3c) of the raw device's mount point.
    u32 simple_umount()
    {
        const u32 r = unmount(ctx, "/dev_hdd0");
        if (r == kEBUSY) {
            module_printf(ctx, "simple_umount: mount_point(%s) is busy\n", {"/dev_hdd0"});
        } else if (r != 0) {
            module_printf(ctx, "simple_umount: cellFsUtilUmount failed[%x]\n", {r});
        }
        return r;
    }

    // fn_f8c4: a descriptor of the device object's closed: the raw device's closes, the mount's unmounts.
    void device_close(s32 fd)
    {
        if (raw() == fd && close_fd(fd) == 0) ps_write32(b, device + 4, 0xFFFFFFFF);
        if (mounted() == fd && simple_umount() == 0) ps_write32(b, device, 0xFFFFFFFF);
    }

    // close (fn_10d28): the raw device's descriptor through the device object (0), another through lv2.
    s32 close(s32 fd)
    {
        if (raw() == fd) {
            device_close(fd);
            return 0;
        }
        const u32 r = close_fd(fd);
        if (r == 0) return 0;
        err = r;
        return -1;
    }

    // ---- the TemporalMount (the raw device mounted for one call)

    // fn_ed80: the path names the hard disk: its partition's name, its raw device, its mount point (the module's table
    // at 0x253cc, its first word set to the device's name on the first call).
    static bool is_disk(std::string_view path)
    {
        return path == "hdd0" || path == "CELL_FS_UTILITY:HDD0" || path == "/dev_hdd0";
    }

    // fn_fad8 (through fn_fc00): the raw device mounted at /dev_hdd0 and opened; the descriptor, or -1.
    s32 open_disk()
    {
        const s32 raw = static_cast<s32>(ps_read32(b, device + 4));
        if (raw != -1) return raw;
        const u32 r = mount(ctx, "CELL_FS_UTILITY:HDD0", "CELL_FS_SIMPLEFS", "/dev_hdd0");
        if (r == 0 || r == kEBUSY || r == kEINVAL) {
            PS_LOG_ERROR(kChan, "fsck: the hard disk mounted: reading it raw is not supported");
            if (r == 0) unmount(ctx, "/dev_hdd0");
        }
        // The module's errno here is a word of its stack it never set (0 when the stack is clean).
        err = 0;
        return -1;
    }

    // ---- files (fn_101cc, fn_fcb4, fn_10d28)

    // stat: the disk through the TemporalMount, an absolute path through sys_fs_stat; a relative one does not exist.
    // False (with errno) when it cannot be had; one that exists is past what is ported (unsupported, exit 8).
    bool stat(u32 path)
    {
        const std::string p = text(path);
        if (is_disk(p)) {
            // fn_ffd8: the disk opened first; the module's errno is then another word of its stack never set.
            if (open_disk() == -1) {
                err = 0;
                return false;
            }
            return false;
        }
        if (p.empty() || p[0] != '/') {
            err = kENOENT;
            return false;
        }
        StackVar sb(ctx, b, 0x40);
        const u32 r = sc(ctx, SC_FS_STAT, {path, sb.addr()});
        if (r != 0) {
            err = r;
            return false;
        }
        unsupported(p);
    }

    // A file or directory that exists: checking it (the module goes on to its mounts, then reads it block by block as
    // a disk) is not supported. No game names one; the module's answer for nearly all of them is 8 too.
    [[noreturn]] void unsupported(const std::string& path)
    {
        PS_LOG_ERROR(kChan, "fsck: {} exists: checking a file or directory as a disk is not supported (exit 8)", path);
        exit(8);
    }

    // open: the disk through the TemporalMount; an absolute path with sys_fs_open; -1 with errno set otherwise.
    s32 open(u32 path, u32 flags)
    {
        const std::string p = text(path);
        if (is_disk(p)) return open_disk();
        if (p.empty() || p[0] != '/') {
            err = kENOENT;
            return -1;
        }
        StackVar fd(ctx, b, 8);
        u32 r = sc(ctx, SC_FS_OPEN, {path, flags, fd.addr(), 0, 0, 0});
        if (r == kEISDIR) r = sc(ctx, SC_FS_OPENDIR, {path, fd.addr()});
        if (r != 0) {
            err = r;
            return -1;
        }
        close_fd(static_cast<s32>(ps_read32(b, fd.addr())));
        unsupported(p);
    }

    // ---- output (liblv2's printf, as the module's)

    // strerror (fn_2088c).
    static const char* strerror(u32 e) { return e == kENOSYS ? "not supported" : "dummy string"; }

    // pwarn (fn_13f98).
    void pwarn(std::string_view fmt, std::initializer_list<sysutil::sub::PrintArg> args)
    {
        if (preen) module_printf(ctx, "%s: ", {text(cdevname)});
        module_printf(ctx, fmt, args);
    }

    // pfatal (fn_15344): the message; when preening, the device's name before it and the inconsistency after it,
    // then ckfini and exit(8). (A background check, the soft updates' message: not reached here.)
    void pfatal(std::string_view fmt, std::initializer_list<sysutil::sub::PrintArg> args)
    {
        if (!preen) {
            module_printf(ctx, fmt, args);
            return;
        }
        const std::string dev = cdevname ? text(cdevname) : std::string("fsck");
        module_printf(ctx, "%s: ", {dev});
        module_printf(ctx, fmt, args);
        module_printf(ctx, "\n%s: UNEXPECTED%sINCONSISTENCY; RUN fsck MANUALLY.\n", {dev, " "});
        ckfini();
        exit(8);
    }

    // ckfini (fn_14e80) with nothing written yet (no write descriptor): the read descriptor closed.
    void ckfini() { close(fsreadfd); }

    // ---- getopt (fn_209c8): Sony's own. The index starts at 0 (argv[0] is the program's name); a word that is not
    // an option of `opts` ends the options (an unknown option too); an option taking an argument takes the next word.
    s32 getopt(s32 argc, u32 argv, std::string_view opts)
    {
        if (!argv) return -1;
        u32 at = argv + static_cast<u32>(optind) * 4;
        if (optind == 0) {
            m.progname = ps_read32(b, at);
            at += 4;
        }
        const s32 i = static_cast<s32>((at - argv) / 4);
        if (argc <= i) {
            optind = i;
            return -1;
        }
        const u32 word = ps_read32(b, at);
        while (word == 0) {
            // A null word before argc: the module loops on it for ever.
            if (!lv2::sleep_us(thread_of(ctx).waiter, 1000)) throw lv2::ThreadExit{};
        }
        const std::string w = text(word);
        if (w.empty() || w[0] != '-') {
            optind = i;
            return -1;
        }
        const char c = w.size() > 1 ? w[1] : 0;
        const usize o = c ? opts.find(c) : opts.size();
        if (o == std::string_view::npos) {
            optind = i;
            return -1;
        }
        const char next = o + 1 < opts.size() ? opts[o + 1] : 0;
        if (next == ':') {
            at += 4;
            m.optarg = ps_read32(b, at);
        }
        optind = static_cast<s32>((at + 4 - argv) / 4);
        return o < opts.size() ? static_cast<s32>(static_cast<s8>(opts[o])) : 0;
    }

    // argtoi (fn_181bc): the module's strtol is a stub that says so and answers 0 without setting the end pointer,
    // which argtoi then reads uninitialised (see the top): here the 0 is taken.
    s32 argtoi(char, std::string_view, u32)
    {
        module_printf(ctx, "%s: to avoid link error for prx\n", {"my_strtol"});
        return 0;
    }

    // blockcheck (fn_20060): a name that does not exist without a '/' is tried as /dev_<name>.
    u32 blockcheck(u32 name)
    {
        u32 newname = name;
        std::optional<GuestText> prefixed; // the module's buffer of "/dev_" and the name
        if (!stat(name) && text(name).find('/') == std::string::npos) {
            prefixed.emplace(ctx, "/dev_" + text(name));
            newname = prefixed->addr();
        }
        if (!stat(newname)) module_printf(ctx, "Can't stat %s: %s\n", {text(newname), strerror(err)});
        return name;
    }

    // sblock_init (fn_1e7e8): the superblock buffers from the heap; the device's sector size (an ioctl the module
    // answers itself with CELL_ENOSYS: 512).
    void sblock_init()
    {
        sblk = heap_malloc(0x2000);
        asblk = heap_malloc(0x2000);
        if (!sblk || !asblk) {
            module_printf(ctx, "cannot allocate space for superblock", {});
            exit(8);
        }
        err = kENOSYS;
    }

    // fn_1ee68: the superblock read once before the checks (Sony's addition).
    void sb_preread(u32 dev)
    {
        fsreadfd = open(dev, 0);
        if (fsreadfd < 0) {
            module_printf(ctx, "Can't sb read open %s\n", {text(dev)});
            return;
        }
    }

    // setup (fn_1f230) of a device that cannot be had: 0.
    s32 setup(u32 dev)
    {
        stat(dev);
        module_printf(ctx, "Can't stat %s: %s\n", {text(dev), strerror(err)});
        return 0;
    }

    // main (fn_18254): options, then each file system named.
    void main(s32 argc, u32 argv)
    {
        skipclean = 1;
        for (s32 ch; (ch = getopt(argc, argv, "b:Bc:dfFm:npy")) != -1;) {
            switch (ch) {
            case 'b':
                skipclean = 0;
                bflag = argtoi('b', "number", m.optarg);
                module_printf(ctx, "Alternate super block location: %d\n", {bflag});
                break;
            case 'B': bkgrdflag = 1; break;
            case 'c':
                skipclean = 0;
                cvtlevel = argtoi('c', "conversion level", m.optarg);
                if (cvtlevel < 3) {
                    module_printf(ctx, "cannot do level %d conversion", {cvtlevel});
                    exit(8);
                }
                break;
            case 'd': debug++; break;
            case 'f': skipclean = 0; break;
            case 'F': bkgrdcheck = 1; break;
            case 'm':
                m.lfmode = argtoi('m', "mode", m.optarg);
                if (m.lfmode & ~0xFFF) {
                    module_printf(ctx, "bad mode to -m: %o", {m.lfmode});
                    exit(8);
                }
                module_printf(ctx, "** lost+found creation mode %o\n", {m.lfmode});
                break;
            case 'n':
                nflag++;
                yflag = 0;
                break;
            case 'p': preen++; break;
            case 'y':
                yflag++;
                nflag = 0;
                break;
            default: exit(1); // usage()
            }
        }
        argc -= optind;
        u32 arg = argv + static_cast<u32>(optind) * 4;
        if (argc == 0) exit(1);
        // signal and getrlimit are the module's stubs (nothing printed); setitimer says what it is.
        if (bkgrdflag) module_printf(ctx, "dummy_entry(%s)\n", {"setitimer"});
        for (; argc > 0; argc--, arg += 4) {
            const u32 filesys = ps_read32(b, arg);
            cdevname = filesys;
            if (debug && preen) pwarn("starting\n", {});
            stat(filesys); // getmntpt: a mount only for a name that exists
            const u32 dev = blockcheck(filesys);
            sblock_init();
            sb_preread(dev);
            if (bkgrdcheck) {
                fsreadfd = open(dev, 0);
                exit(3); // the superblock cannot be read
            }
            if (bkgrdflag) {
                // Not mounted (getmntpt found nothing): no background check.
                bkgrdflag = 0;
                pfatal("NOT MOUNTED, CANNOT RUN IN BACKGROUND\n", {});
            }
            setup(dev); // 0: the device cannot be had
            if (preen) pfatal("CAN'T CHECK FILE SYSTEM.", {});
            exit(8);
        }
        exit(0);
    }
};

// The heap of A9B04535 (fn_ec48): 2 MB from lv2 when -F is among the words, else 32 MB.
bool has_foreground(Fsck& f, s32 argc, u32 argv)
{
    for (s32 i = 0; i < argc; i++)
        if (f.text(ps_read32(f.b, argv + static_cast<u32>(i) * 4)) == "-F") return true;
    return false;
}

// A run: main under the module's setjmp; exit's status is the answer (exit(0) and exit(1) both come back as 1 from
// setjmp: 1 for exit(1), `setup` for exit(0)).
s32 run(Fsck& f, s32 argc, u32 argv, s32 setup)
{
    s32 r = setup;
    try {
        f.main(argc, argv);
    } catch (const Exit& e) {
        r = e.status == 0 || e.status == 1 ? (f.exit_zero ? setup : 1) : e.status;
    }
    return r;
}

s32 fs_utility_init_A9B04535(PPUContext& ctx, s32 argc, u32 argv)
{
    Module& m = module();
    std::lock_guard lock(m.call);
    Fsck f(ctx, m);
    if (!f.create_heap(has_foreground(f, argc, argv) ? 0x200000 : 0x2000000)) return static_cast<s32>(kENOMEM);
    if (!f.create_device()) {
        f.delete_heap();
        return static_cast<s32>(kENOMEM);
    }
    const s32 r = run(f, argc, argv, 0);
    f.finish();
    return r;
}

// The heap of 263172B8 and 1F3CD9F1 (fn_eafc): the caller's memory, at least 2 MB with -F, else 32 MB.
bool caller_heap(Fsck& f, s32 argc, u32 argv, u32 memory, u32 size)
{
    if (size < (has_foreground(f, argc, argv) ? 0x200000u : 0x2000000u)) {
        module_printf(f.ctx, "ERROR fsck_cfs: wrong size(%ld) is specified for heap area\n",
            {static_cast<u64>(static_cast<s64>(static_cast<s32>(size)))});
        return false;
    }
    if (!f.create_heap_at(memory, size)) return false;
    if (!f.create_device()) {
        f.delete_heap();
        return false;
    }
    return true;
}

s32 fs_utility_init_263172B8(PPUContext& ctx, s32 argc, u32 argv, u32 memory, u32 size)
{
    Module& m = module();
    std::lock_guard lock(m.call);
    Fsck f(ctx, m);
    if (!caller_heap(f, argc, argv, memory, size)) return static_cast<s32>(kENOMEM);
    const s32 r = run(f, argc, argv, 0);
    f.finish();
    return r;
}

s32 fs_utility_init_1F3CD9F1(PPUContext& ctx, s32 argc, u32 argv, u32 memory, u32 size, u32 callback, u64 id)
{
    Module& m = module();
    std::lock_guard lock(m.call);
    Fsck f(ctx, m);
    if (!caller_heap(f, argc, argv, memory, size)) return static_cast<s32>(kENOMEM);
    // set_current_callback: one call keeps it at a time (with the calls one at a time here, the module's "already has"
    // never happens).
    m.callback = callback;
    m.notification = id;
    const s32 r = run(f, argc, argv, 0);
    // clear_current_callback.
    if (m.callback == 0) {
        module_printf(ctx, "%s: already cleared current_callback[%p] and current_notification_id[%#llx], "
                           "request_callback[%p], request_notification_id[%#llx]\n",
            {"clear_current_callback", 0u, m.notification, callback, id});
    } else if (m.callback == callback && m.notification == id) {
        m.callback = 0;
        m.notification = 0;
    } else {
        module_printf(ctx, "%s: unexpect current_callback[%p] and current_notification_id[%#llx], "
                           "request_callback[%p], request_notification_id[%#llx]\n",
            {"clear_current_callback", m.callback, m.notification, callback, id});
    }
    f.finish();
    return r;
}

} // namespace

u32 fs_utility_init_disk(PPUContext& ctx)
{
    Module& m = module();
    std::lock_guard lock(m.call);
    return disk_object(ctx, m);
}

void register_fs_utility_init(Registry& r)
{
    r.add("fs_utility_init", {
        PS_HLE_FUNC(0x1F3CD9F1, fs_utility_init_1F3CD9F1),
        PS_HLE_FUNC(0x263172B8, fs_utility_init_263172B8),
        PS_HLE_FUNC(0x4E949DA4, fs_utility_init_4E949DA4),
        PS_HLE_FUNC(0x665DF255, fs_utility_init_665DF255),
        PS_HLE_FUNC(0x6B5896B0, fs_utility_init_6B5896B0),
        PS_HLE_FUNC(0xA9B04535, fs_utility_init_A9B04535),
        PS_HLE_FUNC(0xE7563CE6, fs_utility_init_E7563CE6),
        PS_HLE_FUNC(0xF691D443, fs_utility_init_F691D443),
    });
}

} // namespace pspocket::hle
