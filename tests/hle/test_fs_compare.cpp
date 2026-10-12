// sys_fs against libfs.sprx (D-193's method, D-518): the same scripts of calls go to pspocket's sys_fs and to Sony's
// libfs, each in a fresh process (kernel, HLE and Sony's module loaded in both) over the same freshly made file tree;
// every answer, every byte the calls write and the file tree afterwards (names, kinds, sizes, contents, link targets)
// must be the same. Skipped without the firmware (PSPOCKET_TEST_DEV_FLASH, firmware_compare.hpp); no game data.

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "firmware_compare.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;
namespace fsys = std::filesystem;

namespace {

constexpr u32 kStr = kData + 0x1000;  // path strings, 0x100 apart
constexpr u32 kOut = kData + 0x20000; // what the calls write
constexpr u32 kOutSize = 0x4000;
constexpr u8 kFill = 0xA5;
constexpr u64 kFdMark = 0xFD00000000000000ull; // an argument read at call time: the word at kOut + the low bits

const char* const kPaths[] = {
    "/dev_hdd0/dir/file.bin",             // 0
    "/dev_hdd0/dir/missing.bin",          // 1
    "/dev_hdd0/dir/sub",                  // 2
    "/dev_bdvd/PS3_GAME/USRDIR/data.bin", // 3
    "/dev_hdd0/dir/link.bin",             // 4
    "/",                                  // 5
    "/dev_nothing/x",                     // 6
    "relative",                           // 7
    "/dev_usb000/u.bin",                  // 8
    "/dev_hdd0/dir/empty.bin",            // 9
    "/dev_hdd0/dir/new.bin",              // 10
    "/dev_hdd0",                          // 11
    "/dev_hdd01/x",                       // 12
    "/dev_bdvd/PS3_GAME",                 // 13
    "/dev_usb000/link.bin",               // 14
    "",                                   // 15
    "/dev_hdd0/dir/sub/x.bin",            // 16
    "/dev_hdd0/game/TEST00000/USRDIR",    // 17
    "file.bin",                           // 18
    "/dev_bdvd/PS3_GAME/USRDIR",          // 19
    "/dev_hdd0/dir/sub/../file.bin",      // 20
    "/dev_usb000",                        // 21
    "/dev_hdd0/top.bin",                  // 22: games may not write directly in /dev_hdd0
    "/dev_hdd0/top2.bin",                 // 23
};

u64 S(u32 i) { return kStr + 0x100 * i; }
u64 O(u32 off) { return kOut + off; }
u64 FD(u32 off) { return kFdMark | (kOut + off); }

// A call, or with fn "=" a 64-bit store before the next call (args: address, value).
struct Op {
    const char* fn;
    std::vector<u64> args;
};

// The tree every run starts from: hdd0 (writable, UFS), bdvd (read-only), usb000 (writable, FAT).
struct Tree {
    fsys::path root = fsys::temp_directory_path() / std::format("pspocket-fs-compare-{}", ::getpid());

    ~Tree()
    {
        std::error_code ec;
        fsys::remove_all(root, ec);
    }
    void reset() const
    {
        fsys::remove_all(root);
        fsys::create_directories(root / "hdd0" / "dir" / "sub");
        fsys::create_directories(root / "hdd0" / "game" / "TEST00000" / "USRDIR");
        fsys::create_directories(root / "bdvd" / "PS3_GAME" / "USRDIR");
        fsys::create_directories(root / "usb000");
        write(root / "hdd0" / "dir" / "file.bin", 10000, 1);
        write(root / "hdd0" / "dir" / "empty.bin", 0, 0);
        write(root / "hdd0" / "top.bin", 50, 9);
        write(root / "bdvd" / "PS3_GAME" / "USRDIR" / "data.bin", 9000, 7);
        write(root / "usb000" / "u.bin", 300, 3);
    }
    static void write(const fsys::path& p, usize n, u8 seed)
    {
        std::ofstream out(p, std::ios::binary);
        for (usize i = 0; i < n; i++) out.put(static_cast<char>(static_cast<u8>(i * 31 + seed)));
    }

    // Every entry below the root: kind, size, contents, link target.
    std::map<std::string, std::string> snapshot() const
    {
        std::map<std::string, std::string> out;
        for (const auto& e : fsys::recursive_directory_iterator(root)) {
            const std::string rel = fsys::relative(e.path(), root).string();
            if (e.is_symlink()) {
                out[rel] = "link " + fsys::read_symlink(e.path()).string();
            } else if (e.is_directory()) {
                out[rel] = "dir";
            } else {
                std::ifstream in(e.path(), std::ios::binary);
                out[rel] = "file " + std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            }
        }
        return out;
    }
};

struct Run {
    std::vector<u32> answers;
    std::vector<u8> out;
    std::map<std::string, std::string> tree;
};

// One script in a fresh process: pspocket's functions, or Sony's (`sony`).
Run run(const std::string& flash, const Tree& tree, const std::vector<Op>& ops, bool sony, u32 container_size)
{
    tree.reset();
    Fixture f;
    REQUIRE(f.kernel->vfs_mount("/dev_hdd0", (tree.root / "hdd0").string(), false));
    REQUIRE(f.kernel->vfs_mount("/dev_bdvd", (tree.root / "bdvd").string(), true));
    REQUIRE(f.kernel->vfs_mount("/dev_usb000", (tree.root / "usb000").string(), false));
    GuestThread t(f, 0, "ppu");
    FirmwareLibrary lib(f, t, flash, "sys_fs"); // loaded in both processes, so memory is laid out alike
    u8* base = f.base();
    for (u32 i = 0; i < std::size(kPaths); i++) std::memcpy(base + S(i), kPaths[i], std::strlen(kPaths[i]) + 1);
    std::memset(base + kOut, kFill, kOutSize);
    if (container_size) { // a memory container, its ID at kOut + 0x3FF0
        REQUIRE(lv2::sys_memory_container_create(*t.t, static_cast<u32>(O(0x3FF0)), container_size) == lv2::CELL_OK);
    }
    Run r;
    for (const Op& op : ops) {
        if (std::string_view(op.fn) == "=") {
            ps_write64(base, static_cast<u32>(op.args[0]), op.args[1]);
            r.answers.push_back(0);
            continue;
        }
        std::vector<u64> args = op.args;
        for (u64& a : args)
            if ((a & 0xFF00000000000000ull) == kFdMark) a = ps_read32(base, static_cast<u32>(a));
        u32 r3 = 0;
        if (sony) {
            std::array<u64, 8> a{};
            std::copy(args.begin(), args.end(), a.begin());
            r3 = lib.call(t, recomp::nid_of(op.fn), {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]});
        } else {
            const Function& fn = GuestThread::find(op.fn);
            for (usize i = 0; i < args.size(); i++) t.ctx.gpr[3 + i] = args[i];
            const u64 sp = t.ctx.gpr[1];
            fn.fn(t.ctx, base);
            CHECK(t.ctx.gpr[1] == sp);
            r3 = static_cast<u32>(t.ctx.gpr[3]);
        }
        r.answers.push_back(r3);
    }
    r.out.assign(base + kOut, base + kOut + kOutSize);
    r.tree = tree.snapshot();
    return r;
}

// `skip_out`: output ranges (offset, size) not compared.
void compare(const std::string& flash, const Tree& tree, const std::vector<Op>& ops, const std::string& what,
    u32 container_size = 0, const std::vector<std::pair<u32, u32>>& skip_out = {})
{
    const Run mine = run(flash, tree, ops, false, container_size);
    const Run sonys = run(flash, tree, ops, true, container_size);
    for (usize i = 0; i < ops.size(); i++) {
        INFO(std::format("{}: step {} {} -> ours {:#x}, Sony's {:#x}", what, i, ops[i].fn, mine.answers[i],
            sonys.answers[i]));
        CHECK(mine.answers[i] == sonys.answers[i]);
    }
    std::vector<u8> a = mine.out, b = sonys.out;
    for (const auto& [off, n] : skip_out) {
        std::memset(a.data() + off, 0, n);
        std::memset(b.data() + off, 0, n);
    }
    usize at = 0;
    while (at < a.size() && a[at] == b[at]) at++;
    INFO(std::format("{}: output differs first at +{:#x}", what, at));
    CHECK(at == a.size());
    for (const auto& [name, ours] : mine.tree) {
        INFO(std::format("{}: {}", what, name));
        const auto it = sonys.tree.find(name);
        CHECK(it != sonys.tree.end());
        if (it != sonys.tree.end()) CHECK(ours == it->second);
    }
    CHECK(mine.tree.size() == sonys.tree.size());
}

} // namespace

TEST_CASE("sys_fs against libfs", "[hle][lle][fs]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    const Tree tree;
    const std::string& fl = *flash;

    SECTION("paths: access, ACLs, links, chown")
    {
        std::vector<Op> ops;
        for (const u32 p : {0u, 1u, 2u, 3u, 5u, 6u, 7u, 8u, 11u, 13u, 15u, 20u, 21u, 22u})
            for (const u64 mode : {0ull, 1ull, 2ull, 4ull, 7ull, 8ull, 0xFFFFFFFFull})
                ops.push_back({"cellFsAccess", {S(p), mode}});
        ops.push_back({"cellFsAccess", {0, 0}});
        for (const u32 p : {0u, 1u, 2u, 3u, 5u, 6u, 7u, 8u, 15u}) {
            ops.push_back({"cellFsAclRead", {S(p), O(0x10 * p)}});
            ops.push_back({"cellFsAclRead", {S(p), 0}});
        }
        ops.push_back({"=", {O(0x200), 0x1234567800000000ull}});
        for (const u32 p : {0u, 1u, 3u, 5u, 8u}) ops.push_back({"cellFsAclWrite", {S(p), O(0x200)}});
        ops.push_back({"cellFsAclWrite", {S(1), 0}});
        ops.push_back({"cellFsAclRead", {S(0), O(0x210)}});
        ops.push_back({"cellFsLink", {S(0), S(4)}});  // a new hard link
        ops.push_back({"cellFsLink", {S(0), S(4)}});  // it exists now
        ops.push_back({"cellFsLink", {S(1), S(10)}}); // from a missing file
        ops.push_back({"cellFsLink", {S(2), S(10)}}); // a directory
        ops.push_back({"cellFsLink", {S(0), S(14)}}); // across mounts
        ops.push_back({"cellFsLink", {S(3), S(10)}}); // from the disc
        ops.push_back({"cellFsLink", {S(8), S(14)}}); // FAT
        ops.push_back({"cellFsLink", {S(6), S(10)}});
        ops.push_back({"cellFsLink", {S(0), S(23)}}); // into /dev_hdd0 itself
        ops.push_back({"cellFsLink", {S(22), S(10)}});
        ops.push_back({"cellFsLink", {S(0), 0}});
        ops.push_back({"cellFsLink", {0, S(10)}});
        ops.push_back({"cellFsChown", {S(0), 1, 2}});
        ops.push_back({"cellFsChown", {S(1), 1, 2}});
        ops.push_back({"cellFsChown", {S(3), 0, 0}});
        ops.push_back({"cellFsChown", {S(5), 0, 0}});
        ops.push_back({"cellFsChown", {S(2), 0xFFFFFFFF, 0xFFFFFFFF}});
        compare(fl, tree, ops, "paths");
    }

    SECTION("symbolic links")
    {
        compare(fl, tree,
            {{"cellFsSymbolicLink", {S(0), S(4)}}, {"cellFsSymbolicLink", {S(0), S(4)}},
                {"cellFsSymbolicLink", {S(18), S(10)}}, {"cellFsSymbolicLink", {S(3), S(16)}},
                {"cellFsSymbolicLink", {S(0), S(14)}}, {"cellFsSymbolicLink", {S(0), S(3)}},
                {"cellFsSymbolicLink", {S(15), S(10)}}, {"cellFsSymbolicLink", {S(0), S(6)}},
                {"cellFsSymbolicLink", {0, S(10)}}, {"cellFsSymbolicLink", {S(0), 0}},
                {"cellFsSymbolicLink", {S(0), S(23)}}, {"cellFsSymbolicLink", {S(20), S(16)}},
                {"cellFsStat", {S(4), O(0x100)}}},
            "symbolic links");
    }

    SECTION("opens: cellFsOpen, cellFsOpen2, SDATA with a version")
    {
        std::vector<Op> ops;
        u32 at = 0;
        for (const u64 flags : {0ull, 1ull, 2ull, 0x41ull, 0x42ull, 0xC1ull, 0x241ull, 0x401ull, 3ull}) {
            for (const u32 p : {0u, 1u, 2u, 3u, 10u, 22u, 23u}) {
                for (const u64 mode : {0xFFFFFFFFull, 0ull, 0777ull}) {
                    ops.push_back({"cellFsOpen2", {S(p), flags, O(at), mode, 0, 0}});
                    ops.push_back({"cellFsClose", {FD(at)}});
                    at += 4;
                }
            }
        }
        ops.push_back({"cellFsOpen2", {S(0), 0, 0, 0, 0, 0}});
        ops.push_back({"cellFsOpen", {S(0), 0, O(at), 0, 0}});
        ops.push_back({"cellFsClose", {FD(at)}});
        for (const u64 flags : {0ull, 1ull, 2ull}) {
            for (const u32 p : {0u, 1u, 2u, 3u, 5u}) {
                ops.push_back({"cellFsSdataOpenWithVersion", {S(p), flags, O(0x1000 + at), 7, 0, 0}});
                ops.push_back({"cellFsClose", {FD(0x1000 + at)}});
                at += 4;
            }
        }
        ops.push_back({"cellFsSdataOpenWithVersion", {S(0), 0, 0, 7}});
        ops.push_back({"cellFsSdataOpen", {S(0), 0, O(0x1000 + at), 0, 0}});
        ops.push_back({"cellFsClose", {FD(0x1000 + at)}});
        ops.push_back({"cellFsSdataOpen", {S(0), 2, O(0x1000 + at + 4), 0, 0}});
        compare(fl, tree, ops, "opens");
    }

    SECTION("sizes: the allocate and change-size calls, truncate2, fcntl")
    {
        std::vector<Op> ops = {{"cellFsOpen", {S(0), 2, O(0), 0, 0}}, {"cellFsOpen", {S(9), 0, O(4), 0, 0}},
            {"cellFsOpen", {S(3), 0, O(8), 0, 0}}, {"cellFsOpen", {S(22), 2, O(12), 0, 0}},
            {"cellFsOpen", {S(9), 1, O(16), 0, 0}}};
        for (const char* fn : {"cellFsAllocateFileAreaByFdWithInitialData", "cellFsAllocateFileAreaByFdWithoutZeroFill",
                 "cellFsChangeFileSizeByFdWithoutAllocation", "cellFsTruncate2"}) {
            for (const u64 fd : {FD(0), FD(4), FD(8), FD(12), FD(16), u64{0}, u64{99}, u64{0xFFFFFFFF}})
                for (const u64 size : {u64{0}, u64{100}, u64{5000}}) ops.push_back({fn, {fd, size}});
            ops.push_back({"cellFsFstat", {FD(0), O(0x100)}});
        }
        for (const char* fn : {"cellFsAllocateFileAreaWithInitialData", "cellFsChangeFileSizeWithoutAllocation",
                 "cellFsAllocateFileAreaWithoutZeroFill"}) {
            for (const u32 p : {9u, 1u, 2u, 3u, 5u, 6u, 7u, 8u, 15u, 22u})
                for (const u64 size : {u64{0}, u64{77}, u64{3000}}) ops.push_back({fn, {S(p), size}});
            ops.push_back({fn, {0, 10}});
            ops.push_back({"cellFsStat", {S(9), O(0x200)}});
        }
        ops.push_back({"cellFsFcntl", {FD(0), 0x80000004, O(0x300), 4}});
        ops.push_back({"cellFsFcntl", {FD(0), 0x80000004, O(0x300), 8}});
        ops.push_back({"=", {O(0x310), 64}});
        ops.push_back({"cellFsFcntl", {FD(0), 0x80000006, O(0x310), 4}});
        ops.push_back({"cellFsFcntl", {FD(0), 0x80000006, O(0x310), 8}});
        ops.push_back({"cellFsTruncate", {S(9), 12}});
        ops.push_back({"cellFsFtruncate", {FD(0), 20}});
        for (const u32 o : {0u, 4u, 8u, 12u, 16u}) ops.push_back({"cellFsClose", {FD(o)}});
        compare(fl, tree, ops, "sizes");
    }

    SECTION("block sizes and directory entries")
    {
        std::vector<Op> ops = {{"cellFsOpen", {S(0), 0, O(0), 0, 0}}, {"cellFsOpen", {S(3), 0, O(4), 0, 0}},
            {"cellFsOpendir", {S(2), O(8)}}};
        u32 at = 0x100;
        for (const u64 fd : {FD(0), FD(4), FD(8), u64{0}, u64{77}}) {
            ops.push_back({"cellFsFGetBlockSize2", {fd, O(at), O(at + 8), O(at + 16), O(at + 24)}});
            ops.push_back({"cellFsFGetBlockSize2", {fd, O(at), O(at + 8), O(at + 16), 0}});
            ops.push_back({"cellFsFGetBlockSize", {fd, O(at + 32), O(at + 40)}});
            at += 0x30;
        }
        for (const u32 nul : {1u, 2u, 3u}) {
            ops.push_back({"cellFsFGetBlockSize2", {FD(0), nul == 1 ? 0 : O(at), nul == 2 ? 0 : O(at + 8),
                                                       nul == 3 ? 0 : O(at + 16), O(at + 24)}});
            at += 0x20;
        }
        for (const u32 p : {0u, 2u, 3u, 8u, 11u, 13u, 17u}) {
            ops.push_back({"cellFsGetBlockSize2", {S(p), O(at), O(at + 8), O(at + 16)}});
            ops.push_back({"cellFsGetBlockSize", {S(p), O(at + 24), O(at + 32)}});
            at += 0x30;
        }
        // cellFsGetBlockSize2's outputs after a failure are libfs's stack: not compared.
        const u32 fail_at = at;
        for (const u32 p : {1u, 5u, 6u, 7u}) {
            ops.push_back({"cellFsGetBlockSize2", {S(p), O(at), O(at + 8), O(at + 16)}});
            at += 0x20;
        }
        ops.push_back({"cellFsGetBlockSize2", {0, O(at), O(at + 8), O(at + 16)}});
        ops.push_back({"cellFsGetBlockSize2", {S(0), 0, O(at + 8), O(at + 16)}});
        ops.push_back({"cellFsGetBlockSize2", {S(0), O(at), 0, O(at + 16)}});
        ops.push_back({"cellFsGetBlockSize2", {S(0), O(at), O(at + 8), 0}});
        at += 0x20;
        for (const u32 p : {0u, 1u, 2u}) {
            ops.push_back({"cellFsGetDirent", {S(p), O(at), 0x100, O(at + 0x100)}});
            ops.push_back({"cellFsGetDirent", {S(p), O(at), 0x100, 0}});
            ops.push_back({"cellFsGetDirentCount", {S(p), O(at + 0x108)}});
            ops.push_back({"cellFsGetDirentCount", {S(p), 0}});
            at += 0x110;
        }
        ops.push_back({"cellFsGetDirectoryEntries", {FD(8), O(at), 0x200, O(at + 0x200)}});
        ops.push_back({"cellFsGetDirectoryEntries", {FD(8), O(at), 0x200, O(at + 0x200)}});
        ops.push_back({"cellFsClose", {FD(0)}});
        ops.push_back({"cellFsClose", {FD(4)}});
        ops.push_back({"cellFsClosedir", {FD(8)}});
        compare(fl, tree, ops, "block sizes", 0, {{fail_at, 0x20 * 4}});
    }

    SECTION("LSN reads and mapped buffers")
    {
        std::vector<Op> ops = {{"cellFsOpen", {S(0), 2, O(0), 0, 0}}, {"cellFsOpen", {S(3), 0, O(4), 0, 0}},
            {"cellFsOpen", {S(8), 0, O(8), 0, 0}}, {"cellFsOpen", {S(0), 1, O(12), 0, 0}}};
        for (const u64 fd : {FD(0), FD(4), FD(8), FD(12), u64{0}, u64{55}}) {
            ops.push_back({"cellFsLsnGetCDASize", {fd, O(0x20)}});
            ops.push_back({"cellFsLsnGetCDA", {fd, O(0x30), 4, O(0x28)}});
            ops.push_back({"cellFsLsnLock", {fd}});
            ops.push_back({"cellFsLsnUnlock", {fd}});
            ops.push_back({"cellFsMappedAllocate", {fd, 0x1000, O(0x40)}});
            ops.push_back({"cellFsMappedFree", {fd, FD(0x40)}});
            ops.push_back({"cellFsMappedFree", {fd, FD(0x40)}});
            ops.push_back({"cellFsMappedAllocate", {fd, 1, 0}});
            ops.push_back({"cellFsMappedFree", {fd, 0}});
            ops.push_back({"cellFsMappedFree", {fd, 0x10000}});
        }
        // ctrl blocks {lsn, offset, size} at +0x80, reads into +0x100.. (0x800 apart).
        const u64 ctrls[][3] = {{0, 0, 100}, {1, 10, 600}, {2, 0, 2048}, {0, 4000, 100}, {100, 0, 10}, {0, 0, 0},
            {0, 0x100, 0x700}};
        u32 dst = 0x100;
        for (const u64 fd : {FD(0), FD(4), FD(8), FD(12), u64{55}}) {
            for (const auto& c : ctrls) {
                ops.push_back({"=", {O(0x80), c[0]}});
                ops.push_back({"=", {O(0x88), c[1]}});
                ops.push_back({"=", {O(0x90), c[2]}});
                ops.push_back({"cellFsLsnRead", {fd, O(0x80), O(dst)}});
                dst = dst + 0x800 >= 0x3F00 ? 0x100 : dst + 0x800;
            }
            ops.push_back({"cellFsLsnRead", {fd, 0, O(dst)}});
            ops.push_back({"cellFsLsnRead", {fd, O(0x80), 0}});
            ops.push_back({"cellFsLsnRead2", {fd, 0, O(dst)}});
            ops.push_back({"cellFsLsnRead2", {fd, O(0x80), 0}});
        }
        for (const u32 o : {0u, 4u, 8u, 12u}) ops.push_back({"cellFsClose", {FD(o)}});
        compare(fl, tree, ops, "lsn calls");
    }

    SECTION("attributes")
    {
        std::vector<Op> ops;
        // A CellFsStat: mode, uid, gid, atime, mtime, ctime, size, blksize.
        const u32 st = 0x100;
        ops.push_back({"cellFsStat", {S(0), O(st)}});
        ops.push_back({"=", {O(st + 12), 1000000000}});
        ops.push_back({"=", {O(st + 20), 1100000000}});
        for (const u32 p : {0u, 2u, 1u, 3u, 8u, 11u, 12u, 17u, 5u, 15u, 22u}) {
            ops.push_back({"cellFsSetAttribute", {S(p), O(st)}});
            ops.push_back({"cellFsSetAttribute", {S(p), 0}});
        }
        ops.push_back({"cellFsSetAttribute", {0, O(st)}});
        ops.push_back({"cellFsStat", {S(0), O(st + 0x40)}});
        // The ctimes not compared: the host's clock when a run last changed the file (the first run's SetAttribute
        // is what the second run's first Stat sees), which a second boundary between them makes differ (D-928).
        compare(fl, tree, ops, "attributes", 0, {{st + 0x1C, 8}, {st + 0x40 + 0x1C, 8}});
    }

    SECTION("I/O buffers and the default container")
    {
        const u64 ct = FD(0x3FF0);
        std::vector<Op> ops = {{"cellFsOpen", {S(0), 0, O(0), 0, 0}}, {"cellFsOpen", {S(3), 0, O(4), 0, 0}}};
        for (const u64 pt : {0ull, 1ull, 2ull, 3ull, 4ull, 5ull, 0x12ull, 0x4002ull}) {
            ops.push_back({"cellFsSetIoBufferFromDefaultContainer", {FD(0), 0x10000, pt}});
            ops.push_back({"cellFsSetIoBuffer", {FD(0), 0x10000, pt, ct}});
            ops.push_back({"cellFsSetIoBuffer", {FD(0), 0, pt, 0xFFFFFFFF}});
        }
        ops.push_back({"cellFsSetIoBuffer", {FD(0), 0x10000, 2, 0xFFFFFFFF}});
        ops.push_back({"cellFsSetIoBuffer", {2, 0x10000, 2, ct}});
        ops.push_back({"cellFsSetIoBuffer", {256, 0x10000, 2, ct}});
        ops.push_back({"cellFsSetIoBuffer", {77, 0x10000, 2, ct}});
        ops.push_back({"cellFsSetIoBuffer", {FD(4), 0x30000, 2, ct}});
        ops.push_back({"cellFsSetIoBuffer", {FD(0), 0x200000, 4, ct}});   // more than the container has
        ops.push_back({"cellFsSetIoBuffer", {FD(0), 0x20000, 2, 0x1234}}); // no such container
        ops.push_back({"cellFsSetDefaultContainer", {ct, 0x20000}});
        ops.push_back({"cellFsSetIoBufferFromDefaultContainer", {FD(0), 0x10000, 2}});
        ops.push_back({"cellFsSetIoBufferFromDefaultContainer", {FD(4), 0x20000, 2}}); // over the default's limit
        ops.push_back({"cellFsSetIoBufferFromDefaultContainer", {FD(4), 0x10000, 2}});
        ops.push_back({"cellFsSetIoBufferFromDefaultContainer", {FD(4), 0x8000, 2}}); // rounds to nothing
        ops.push_back({"cellFsSetIoBufferFromDefaultContainer", {3, 0x10000, 2}});
        ops.push_back({"cellFsSetDefaultContainer", {0, 0}});
        ops.push_back({"cellFsSetIoBufferFromDefaultContainer", {FD(0), 0x10000, 2}});
        ops.push_back({"cellFsClose", {FD(0)}});
        ops.push_back({"cellFsClose", {FD(4)}});
        for (const u64 type : {0ull, 1ull, 2ull, 0xFFFFFFFFull}) ops.push_back({"cellFsSetDiscReadRetrySetting", {type}});
        ops.push_back({"cellFsArcadeHddSerialNumber", {O(0x200)}});
        ops.push_back({"cellFsArcadeHddSerialNumber", {0}});
        compare(fl, tree, ops, "I/O buffers", 0x100000);
    }

    SECTION("conversion callbacks")
    {
        std::vector<Op> ops = {{"cellFsUnregisterL10nCallbacks", {}}};
        for (const u64 cp : {437ull, 437ull, 850ull, 932ull, 0ull, 1252ull})
            ops.push_back({"cellFsRegisterConversionCallback", {cp, 0x1000 + cp, 0x2000 + cp}});
        for (u64 cp = 2000; cp < 2032; cp++) ops.push_back({"cellFsRegisterConversionCallback", {cp, 1, 2}});
        ops.push_back({"cellFsUnregisterL10nCallbacks", {}});
        for (u64 cp = 3000; cp < 3010; cp++) ops.push_back({"cellFsRegisterConversionCallback", {cp, 1, 2}});
        ops.push_back({"cellFsRegisterConversionCallback", {2005, 1, 2}});
        compare(fl, tree, ops, "conversion callbacks");
    }
}
