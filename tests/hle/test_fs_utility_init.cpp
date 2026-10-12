// fs_utility_init (D-935): pspocket's own against Sony's libfs_utility_init (sysutil_sub_compare.hpp: the same script in
// two processes, Sony's module loaded in both; the TTY's text compared too), and what a game's process meets without the
// firmware. No game data.

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>
#include <format>
#include <string>
#include <vector>

#include "fs_utility_init.hpp" // runtime/hle/src
#include "hle_test_util.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/process.hpp"
#include "sysutil_sub_compare.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::sub;

namespace {

constexpr u32 kArgv = kData + 0x0800; // argument vectors (32-bit pointers)
constexpr u32 kStr = kData + 0x1000;  // their strings, 0x40 apart
constexpr u32 kOut = kData + 0x2000;  // outputs

// argv[] at kArgv from `args`, the strings from kStr.
u32 argv(Script& s, const std::vector<std::string>& args)
{
    for (usize i = 0; i < args.size(); i++) {
        s.str(kStr + 0x40 * static_cast<u32>(i), args[i]);
        s.w32(kArgv + 4 * static_cast<u32>(i), kStr + 0x40 * static_cast<u32>(i));
    }
    s.w32(kArgv + 4 * static_cast<u32>(args.size()), 0);
    return static_cast<u32>(args.size());
}

// The disk object of each run, named alike in the lv2 calls: Sony's module keeps it (made by its module_start) at
// 0x279d0, its data segment's (at 0x25260) +0x2770.
void name_disk(Script& s)
{
    const u32 disk = s.sony() ? s.r32(static_cast<u32>(s.module().segments[1].first) + 0x2770)
                              : fs_utility_init_disk(s.thread().ctx);
    REQUIRE(disk);
    s.name_region(disk, kFsUtilityInitDiskSize, "disk");
}

// An lv2 call of the script's (memory for a heap), its own step.
u32 script_sc(Script& s, u32 n, std::initializer_list<u64> args, const char* what)
{
    const u32 r = static_cast<u32>(lv2_syscall(s.thread().ctx, n, args));
    s.step(std::format("{} -> {:#x}", what, r));
    return r;
}

void script_fs_utility(Script& s)
{
    s.compare_tty();
    name_disk(s);
    s.call("fs_utility_init_665DF255", {0});
    s.call("fs_utility_init_665DF255", {kOut});
    s.call("fs_utility_init_E7563CE6", {0});
    s.call("fs_utility_init_E7563CE6", {kOut + 0x40});
    s.call("fs_utility_init_6B5896B0", {0});
    s.call("fs_utility_init_6B5896B0", {kOut + 0x80});
    s.call("fs_utility_init_4E949DA4", {});
    // Memory for the heaps: 2 MB in the script's region, 32 MB from lv2.
    constexpr u32 kHeap2 = kData + 0x100000;
    script_sc(s, 348, {0x2000000, 0x400, kOut + 0x100}, "sys_memory_allocate 32 MB");
    const u32 heap32 = s.r32(kOut + 0x100);
    u32 n = argv(s, {"fsck", "-F", "/dev_hdd0"});
    s.call("fs_utility_init_A9B04535", {n, kArgv});
    s.call("fs_utility_init_A9B04535", {n, kArgv});
    s.call("fs_utility_init_263172B8", {n, kArgv, kHeap2, 0x100000});
    s.call("fs_utility_init_263172B8", {n, kArgv, 0xFFFFFFFF, 0x200000});
    s.call("fs_utility_init_263172B8", {n, kArgv, kHeap2, 0x200000});
    s.call("fs_utility_init_1F3CD9F1", {n, kArgv, kHeap2, 0x200000, 0, 0x1234});
    s.call("fs_utility_init_1F3CD9F1", {n, kArgv, kHeap2, 0x200000, 0x1000, 0x1234});
    n = argv(s, {"fsck", "/dev_hdd0"});
    s.call("fs_utility_init_A9B04535", {n, kArgv});
    s.call("fs_utility_init_263172B8", {n, kArgv, kHeap2, 0x200000});
    s.call("fs_utility_init_263172B8", {n, kArgv, heap32, 0x2000000});
    n = argv(s, {"fsck", "-y", "-F", "/dev_hdd1"});
    s.call("fs_utility_init_A9B04535", {n, kArgv});
    n = argv(s, {"fsck", "-Q", "-F"});
    s.call("fs_utility_init_A9B04535", {n, kArgv});
    n = argv(s, {"fsck", "-F"});
    s.call("fs_utility_init_A9B04535", {n, kArgv});
    s.call("fs_utility_init_A9B04535", {0, kArgv});
    // Preening: CAN'T CHECK through pfatal, whose ckfini closes the device (an unmount of /dev_hdd0 that fails).
    n = argv(s, {"fsck", "-p", "/dev_hdd0"});
    s.call("fs_utility_init_A9B04535", {n, kArgv});
    n = argv(s, {"fsck", "-d", "-p", "-F", "/dev_hdd0"});
    s.call("fs_utility_init_A9B04535", {n, kArgv});
    n = argv(s, {"fsck", "-d", "-p", "/nowhere"});
    s.call("fs_utility_init_A9B04535", {n, kArgv});
    n = argv(s, {"fsck", "-n", "-f", "-y", "/nowhere"});
    s.call("fs_utility_init_A9B04535", {n, kArgv});
    // A background check that is not mounted.
    n = argv(s, {"fsck", "-B", "/dev_hdd0"});
    s.call("fs_utility_init_A9B04535", {n, kArgv});
    n = argv(s, {"fsck", "-B", "-p", "/dev_hdd0"});
    s.call("fs_utility_init_A9B04535", {n, kArgv});
    // The raw device by its other name, a name /dev_ makes the disk, two file systems, odd options.
    n = argv(s, {"fsck", "CELL_FS_UTILITY:HDD0"});
    s.call("fs_utility_init_A9B04535", {n, kArgv});
    n = argv(s, {"fsck", "hdd0"});
    s.call("fs_utility_init_A9B04535", {n, kArgv});
    n = argv(s, {"fsck", "-F", "hdd1", "/dev_hdd0"});
    s.call("fs_utility_init_A9B04535", {n, kArgv});
    n = argv(s, {"fsck", "-", "/dev_hdd0"});
    s.call("fs_utility_init_A9B04535", {n, kArgv});
    n = argv(s, {"fsck", "-:", "/dev_hdd0"});
    s.call("fs_utility_init_A9B04535", {n, kArgv});
    n = argv(s, {"fsck", "-x", "/dev_hdd0"});
    s.call("fs_utility_init_A9B04535", {n, kArgv});
    n = argv(s, {"fsck", "-y", "-Fx", "/dev_hdd0"});
    s.call("fs_utility_init_A9B04535", {n, kArgv});
    n = argv(s, {"fsck"});
    s.call("fs_utility_init_A9B04535", {n, kArgv});
    s.call("fs_utility_init_263172B8", {n, kArgv, kHeap2, 0x200000});
    s.call("fs_utility_init_263172B8", {n, kArgv, heap32, 0x2000000});
    n = argv(s, {"growfs", "/dev_hdd0"});
    s.call("fs_utility_init_F691D443", {n, kArgv, kHeap2, 0x200000});
    s.call("fs_utility_init_F691D443", {0, 0, 0, 0});
    s.call("fs_utility_init_4E949DA4", {});
    s.call("fs_utility_init_6B5896B0", {kOut + 0x88});
    s.call("fs_utility_init_665DF255", {kOut + 0x100});
}

} // namespace

TEST_CASE("fs_utility_init against libfs_utility_init", "[hle][lle]")
{
    compare_script("fs_utility_init", {"fs_utility_init"}, script_fs_utility);
}

namespace {

// The TTY's lines (sys_tty_write logs them on the "tty" channel), the other lines on stderr.
struct TtyLines {
    std::vector<std::string> lines;
    static void sink(void* user, log::Level level, std::string_view channel, std::string_view message)
    {
        if (channel == "tty") static_cast<TtyLines*>(user)->lines.emplace_back(message);
        const std::string_view l = log::level_name(level);
        std::fprintf(stderr, "[%.*s] %.*s: %.*s\n", static_cast<int>(l.size()), l.data(),
            static_cast<int>(channel.size()), channel.data(), static_cast<int>(message.size()), message.data());
    }
    TtyLines() { log::set_sink(&sink, this); }
    ~TtyLines() { log::set_sink(nullptr, nullptr); }
    std::vector<std::string> take() { return std::exchange(lines, {}); }
};

// argv[] at kArgv from `args`.
u32 put_argv(u8* b, const std::vector<std::string>& args)
{
    for (usize i = 0; i < args.size(); i++) {
        const u32 at = kStr + 0x40 * static_cast<u32>(i);
        std::memcpy(b + at, args[i].data(), args[i].size());
        b[at + args[i].size()] = 0;
        ps_write32(b, kArgv + 4 * static_cast<u32>(i), at);
    }
    ps_write32(b, kArgv + 4 * static_cast<u32>(args.size()), 0);
    return static_cast<u32>(args.size());
}

} // namespace

TEST_CASE("fs_utility_init in a game's process: the disk refused, fsck's answers and messages", "[hle]")
{
    TtyLines tty;
    Fixture f;
    REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    constexpr u32 kEFAULT = 0x8001000D, kENOSYS = 0x80010003, kENODEV = 0x8001002D, kEPERM = 0x80010009,
                  kENOMEM = 0x80010004, kEIO = 0x8001002B;

    // The disk's serial number and model: sys_storage is a root process's; the output is left alone.
    CHECK(t.call("fs_utility_init_665DF255", 0) == r3_of_code(kEFAULT));
    std::memset(b + kOut, 0x55, 0x40);
    CHECK(t.call("fs_utility_init_665DF255", kOut) == r3_of_code(kENOSYS));
    CHECK(t.call("fs_utility_init_E7563CE6", kOut) == r3_of_code(kENOSYS));
    CHECK(b[kOut] == 0x55);
    // The partitions: no information, none.
    ps_write64(b, kOut, 7);
    CHECK(t.call("fs_utility_init_6B5896B0", kOut) == r3_of_code(kENODEV));
    CHECK(ps_read64(b, kOut) == 0);
    // The raw device's mount is refused.
    CHECK(t.call("fs_utility_init_4E949DA4") == r3_of_code(kEPERM));
    CHECK(tty.take().empty());

    // fsck: -F asks whether a background check can run (3: the superblock cannot be read).
    u32 n = put_argv(b, {"fsck", "-F", "/dev_hdd0"});
    CHECK(t.call("fs_utility_init_A9B04535", n, kArgv) == 3);
    CHECK(tty.take() == std::vector<std::string>{"Can't stat /dev_hdd0: dummy string", "Can't sb read open /dev_hdd0"});
    // Without it the check cannot start (8); preening, the inconsistency is reported and the device closed.
    n = put_argv(b, {"fsck", "/dev_hdd0"});
    CHECK(t.call("fs_utility_init_A9B04535", n, kArgv) == 8);
    CHECK(tty.take() == std::vector<std::string>{"Can't stat /dev_hdd0: dummy string", "Can't sb read open /dev_hdd0",
                            "Can't stat /dev_hdd0: dummy string"});
    n = put_argv(b, {"fsck", "-p", "/nowhere"});
    CHECK(t.call("fs_utility_init_A9B04535", n, kArgv) == 8);
    // (Each write is a line of the log: the device's name, the message, the inconsistency; ckfini's unmount of the
    // mount point, which is not mounted.)
    CHECK(tty.take() == std::vector<std::string>{"Can't stat /nowhere: dummy string", "Can't sb read open /nowhere",
                            "Can't stat /nowhere: dummy string", "/nowhere: ", "CAN'T CHECK FILE SYSTEM.", "",
                            "/nowhere: UNEXPECTED INCONSISTENCY; RUN fsck MANUALLY.",
                            "simple_umount: cellFsUtilUmount failed[80010002]"});
    // No file system named: usage (1); argc 0: nothing to do (0). The caller's heap memory: 2 MB with -F at least.
    n = put_argv(b, {"fsck"});
    CHECK(t.call("fs_utility_init_A9B04535", n, kArgv) == 1);
    CHECK(t.call("fs_utility_init_A9B04535", 0, kArgv) == 0);
    n = put_argv(b, {"fsck", "-F", "/dev_hdd0"});
    CHECK(t.call("fs_utility_init_263172B8", n, kArgv, kData + 0x100000, 0x100000) == r3_of_code(kENOMEM));
    CHECK(tty.take() == std::vector<std::string>{"ERROR fsck_cfs: wrong size(1048576) is specified for heap area"});
    CHECK(t.call("fs_utility_init_1F3CD9F1", n, kArgv, kData + 0x100000, 0x200000, 0x2000, 0x99) == 3);
    tty.take();
    // A path that exists is not checked (logged): 8.
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "fs_utility_init_hdd0";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "dir");
    REQUIRE(f.kernel->vfs_mount("/dev_hdd0", root.string(), false));
    n = put_argv(b, {"fsck", "/dev_hdd0/dir"});
    CHECK(t.call("fs_utility_init_A9B04535", n, kArgv) == 8);
    std::filesystem::remove_all(root);
    // growfs: the raw device's mount is refused.
    tty.take();
    n = put_argv(b, {"growfs", "/dev_hdd0"});
    CHECK(t.call("fs_utility_init_F691D443", n, kArgv, kData + 0x100000, 0x200000) == r3_of_code(kEIO));
    CHECK(tty.take() == std::vector<std::string>{"Error Status: 1", "Mount failed 0x80010009", ""});
}
