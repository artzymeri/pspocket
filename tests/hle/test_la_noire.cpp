// L.A. Noire (BLES00933): the functions its first boot on the phone stopped on (D-165), each against RPCS3's behaviour.
// Everything is synthetic: the encrypted SDATA file is built here (tests/common/synthetic.cpp build_sdata) with the
// build's NPD keys, which never leave the process; no game data (CLAUDE.md rule 1).

#include "hle_test_util.hpp"

#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "pspocket/core/memory_map.h"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/npd.hpp"
#include "pspocket/rsx/gcm.hpp"
#include "pspocket/rsx/rsx.hpp"
#include "synthetic.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace fsys = std::filesystem;

namespace {

constexpr u32 kStrings = kData + 0x20000, kOut = kData + 0x40000, kBuf = kData + 0x60000;

// Guest code the HLE calls back into runs on the PPU interpreter while this lives.
struct InterpretMisses {
    const ps_ppu_miss_policy saved = ps_ppu_get_miss_policy();
    InterpretMisses() { ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET); }
    ~InterpretMisses() { ps_ppu_set_miss_policy(saved); }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

void put(u8* base, u32 addr, std::string_view s)
{
    std::memcpy(base + addr, s.data(), s.size());
    base[addr + s.size()] = 0;
}

// A guest function at `code` with its descriptor at `opd`: counter += 1, then counter[1] = r3.
void write_counter_function(u8* b, u32 code, u32 opd, u32 counter)
{
    const u32 words[] = {
        0x3CA00000u | (counter >> 16),    // lis r5, counter@h
        0x60A50000u | (counter & 0xFFFF), // ori r5, r5, counter@l
        0x80850000u,                      // lwz r4, 0(r5)
        0x38840001u,                      // addi r4, r4, 1
        0x90850000u,                      // stw r4, 0(r5)
        0x90650004u,                      // stw r3, 4(r5)
        0x4E800020u,                      // blr
    };
    for (u32 i = 0; i < std::size(words); i++) ps_write32(b, code + 4 * i, words[i]);
    ps_write32(b, opd, code);
    ps_write32(b, opd + 4, 0);
}

fsys::path temp_root(std::string_view what)
{
    const fsys::path root = fsys::temp_directory_path() /
        std::format("pspocket-lanoire-{}-{}", what, static_cast<long>(getpid()));
    fsys::remove_all(root);
    fsys::create_directories(root);
    return root;
}

void write_file(const fsys::path& path, const std::vector<u8>& bytes)
{
    fsys::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

} // namespace

// ------------------------------------------------------------------------------------------------- sys_fs

TEST_CASE("cellFsSdataOpen: an encrypted SDATA file on the disc reads as its plain bytes", "[hle][fs][npd][lanoire]")
{
    const recomp::NpdKeys& keys = recomp::builtin_npd_keys();
    if (!keys.present) SKIP("this build has no NPD keys (configured without RPCS3's source)");
    std::vector<u8> plain(70000);
    for (usize i = 0; i < plain.size(); i++) plain[i] = static_cast<u8>(i * 7 + (i >> 9));
    const std::vector<u8> sdata = test::build_sdata(plain, keys, {.version = 3, .extra_flags = 0, .block_size = 0x8000});
    REQUIRE(!sdata.empty());
    std::vector<u8> damaged = sdata;
    damaged[0x100 + 3 * 0x10 + 0x40] ^= 1; // the first block's data

    const fsys::path root = temp_root("sdata");
    write_file(root / "bdvd" / "PS3_GAME" / "USRDIR" / "programs.fp.ps3.sdat", sdata);
    write_file(root / "bdvd" / "PS3_GAME" / "USRDIR" / "damaged.sdat", damaged);
    write_file(root / "bdvd" / "PS3_GAME" / "USRDIR" / "plain.sdat", {'p', 'l', 'a', 'i', 'n'});
    Fixture f;
    u8* b = f.base();
    REQUIRE(f.kernel->vfs_mount("/dev_bdvd", (root / "bdvd").string(), true));
    GuestThread a(f, 0, "a");
    constexpr u32 kPath = kStrings, kFd = kOut, kNread = kOut + 8, kStat = kOut + 0x100;

    put(b, kPath, "/dev_bdvd/PS3_GAME/USRDIR/programs.fp.ps3.sdat");
    CHECK(a.call("cellFsSdataOpen", kPath, 2 /* RDWR */, kFd, 0, 0) == r3_of(lv2::CELL_EINVAL));
    CHECK(a.call("cellFsSdataOpen", kPath, 0, 0, 0, 0) == r3_of(lv2::CELL_EFAULT));
    REQUIRE(a.call("cellFsSdataOpen", kPath, 0, kFd, 0, 0) == 0);
    const u32 fd = ps_read32(b, kFd);
    REQUIRE(a.call("cellFsFstat", fd, kStat) == 0);
    CHECK(ps_read64(b, kStat + 36) == plain.size()); // the plain size
    REQUIRE(a.call("cellFsRead", fd, kBuf, 0x20000, kNread) == 0);
    REQUIRE(ps_read64(b, kNread) == plain.size());
    CHECK(std::memcmp(b + kBuf, plain.data(), plain.size()) == 0);
    REQUIRE(a.call("cellFsClose", fd) == 0);

    // A damaged file does not open (RPCS3: ReadHeader decrypts the first block); a file that is not encrypted opens
    // as it is.
    put(b, kPath, "/dev_bdvd/PS3_GAME/USRDIR/damaged.sdat");
    CHECK(a.call("cellFsSdataOpen", kPath, 0, kFd, 0, 0) == r3_of(lv2::CELL_EFSSPECIFIC));
    put(b, kPath, "/dev_bdvd/PS3_GAME/USRDIR/plain.sdat");
    REQUIRE(a.call("cellFsSdataOpen", kPath, 0, kFd, 0, 0) == 0);
    REQUIRE(a.call("cellFsRead", ps_read32(b, kFd), kBuf, 100, kNread) == 0);
    CHECK(ps_read64(b, kNread) == 5);
    fsys::remove_all(root);
}

// ------------------------------------------------------------------------------------------------- sysPrxForUser

TEST_CASE("sys_ppu_thread_once: the init function runs once per control word", "[hle][lanoire]")
{
    InterpretMisses interpret;
    Fixture f;
    u8* b = f.base();
    GuestThread a(f, 0, "a");
    constexpr u32 kCode = kData + 0x1000, kOpd = kData + 0x1100, kCounter = kData + 0x1200, kOnce = kData + 0x1300;
    write_counter_function(b, kCode, kOpd, kCounter);
    ps_write32(b, kOnce, 0);     // SYS_PPU_THREAD_ONCE_INIT
    ps_write32(b, kOnce + 4, 0); // a second control word
    a.call("sys_ppu_thread_once", kOnce, kOpd);
    CHECK(ps_read32(b, kCounter) == 1);
    CHECK(ps_read32(b, kOnce) == 1); // SYS_PPU_THREAD_DONE_INIT
    a.call("sys_ppu_thread_once", kOnce, kOpd);
    CHECK(ps_read32(b, kCounter) == 1);
    a.call("sys_ppu_thread_once", kOnce + 4, kOpd);
    CHECK(ps_read32(b, kCounter) == 2);

    // Threads racing for one control word: one of them runs it, the others wait for it.
    ps_write32(b, kOnce + 8, 0);
    std::vector<std::thread> threads;
    for (u32 i = 1; i <= 3; i++) {
        threads.emplace_back([&f, i] {
            GuestThread g(f, i, "racer");
            const hle::Function& fn = GuestThread::find("sys_ppu_thread_once");
            g.ctx.gpr[3] = kOnce + 8;
            g.ctx.gpr[4] = kOpd;
            fn.fn(g.ctx, f.base());
        });
    }
    for (auto& t : threads) t.join();
    CHECK(ps_read32(b, kCounter) == 3);
    CHECK(ps_read32(b, kOnce + 8) == 1);
}

// ------------------------------------------------------------------------------------------------- cellRtc, sceNp

TEST_CASE("cellRtcGetCurrentClock: the current time moved by the time zone given", "[hle][rtc][lanoire]")
{
    Fixture f;
    u8* b = f.base();
    GuestThread a(f, 0, "a");
    constexpr u32 kClock = kOut, kClock2 = kOut + 0x20, kTick = kOut + 0x40, kTick2 = kOut + 0x48;
    CHECK(a.call("cellRtcGetCurrentClock", 0, 0) == r3_of_code(0x80010602)); // INVALID_POINTER
    REQUIRE(a.call("cellRtcGetCurrentClock", kClock, 0) == 0);
    REQUIRE(a.call("cellRtcGetCurrentClock", kClock2, 90) == 0);
    REQUIRE(a.call("cellRtcGetTick", kClock, kTick) == 0);
    REQUIRE(a.call("cellRtcGetTick", kClock2, kTick2) == 0);
    const s64 apart = static_cast<s64>(ps_read64(b, kTick2) - ps_read64(b, kTick)); // microseconds
    CHECK(apart >= 90 * 60 * 1'000'000ll - 2'000'000);
    CHECK(apart <= 90 * 60 * 1'000'000ll + 2'000'000);
    // Close to cellRtcGetCurrentTick's UTC.
    REQUIRE(a.call("cellRtcGetCurrentTick", kTick2) == 0);
    const s64 now = static_cast<s64>(ps_read64(b, kTick2) - ps_read64(b, kTick));
    CHECK(now >= 0);
    CHECK(now <= 2'000'000);
}

TEST_CASE("sceNpInit: RPCS3's checks, then NP is up (offline)", "[hle][np][lanoire]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    CHECK(a.call("sceNpInit", 0, kBuf) == r3_of_code(0x8002AA03));        // no pool: INVALID_ARGUMENT
    CHECK(a.call("sceNpInit", 0x20000, 0) == r3_of_code(0x8002AA03));
    CHECK(a.call("sceNpInit", 0x1FFFF, kBuf) == r3_of_code(0x8002AA0F)); // under 128 KB: INSUFFICIENT_BUFFER
    REQUIRE(a.call("sceNpInit", 0x20000, kBuf) == 0);
    CHECK(a.call("sceNpInit", 0x20000, kBuf) == r3_of_code(0x8002AA02)); // ALREADY_INITIALIZED
}

// ------------------------------------------------------------------------------------------------- lv2

TEST_CASE("sys_spu_thread_group_log: on by default, switched off and read back", "[hle][lv2][lanoire]")
{
    Fixture f;
    u8* b = f.base();
    GuestThread a(f, 0, "a");
    lv2::PpuThread& t = *a.t;
    REQUIRE(lv2::sys_spu_thread_group_log(t, 2, kOut) == lv2::CELL_OK);
    CHECK(ps_read32(b, kOut) == 0);
    REQUIRE(lv2::sys_spu_thread_group_log(t, 1, 0) == lv2::CELL_OK);
    REQUIRE(lv2::sys_spu_thread_group_log(t, 2, kOut) == lv2::CELL_OK);
    CHECK(ps_read32(b, kOut) == 1);
    CHECK(lv2::sys_spu_thread_group_log(t, 2, 0) == lv2::err(lv2::CELL_EFAULT));
    CHECK(lv2::sys_spu_thread_group_log(t, 3, kOut) == lv2::err(lv2::CELL_EINVAL));
    REQUIRE(lv2::sys_spu_thread_group_log(t, 0, 0) == lv2::CELL_OK);
    REQUIRE(lv2::sys_spu_thread_group_log(t, 2, kOut) == lv2::CELL_OK);
    CHECK(ps_read32(b, kOut) == 0);
}

// ------------------------------------------------------------------------------------------------- cellGcmSys

namespace {

constexpr u32 kIo = 0x00200000;   // 1 MB of main memory mapped at IO offset 0
constexpr u32 kCtxPtr = kData;    // CellGcmContextData*
constexpr u32 kCmdSize = 0x40000; // 8 segments of 32 KB
constexpr u64 kSecond = 1'000'000'000ull;

lv2::Config rsx_test_config()
{
    lv2::Config c;
    c.rsx_log_failures = true;
    return c;
}

struct GcmFixture : Fixture {
    GcmFixture() : Fixture({}, rsx_test_config())
    {
        REQUIRE(hle->link_internal_functions({"cellGcmSys"}));
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
    }
};

} // namespace

TEST_CASE("cellGcmSys: system mode 512 MB IO, flip and user handlers on the interrupt thread", "[hle][gcm][lanoire]")
{
    InterpretMisses interpret;
    GcmFixture f;
    GuestThread g(f, 0, "gcm");
    u8* b = f.base();
    constexpr u32 kFlipCode = kData + 0x1000, kFlipOpd = kData + 0x1100, kFlipCounter = kData + 0x1200;
    constexpr u32 kUserCode = kData + 0x1400, kUserOpd = kData + 0x1500, kUserCounter = kData + 0x1600;
    write_counter_function(b, kFlipCode, kFlipOpd, kFlipCounter);
    write_counter_function(b, kUserCode, kUserOpd, kUserCounter);

    // Before cellGcmInit the handlers are ignored (RPCS3); the system mode is kept for it.
    g.call("cellGcmSetFlipHandler", kFlipOpd);
    g.call("cellGcmSetUserHandler", kUserOpd);
    REQUIRE(g.call("cellGcmInitSystemMode", 1) == 0); // CELL_GCM_SYSTEM_MODE_IOMAP_512MB
    REQUIRE(g.call("_cellGcmInitBody", kCtxPtr, kCmdSize, 0x100000, kIo) == 0);
    // An IO offset past 256 MB maps in the 512 MB IO address space.
    REQUIRE(f.mem.commit(0x04000000, 0x100000));
    CHECK(g.call("cellGcmMapEaIoAddressWithFlags", 0x04000000, 0x18000000, 0x100000, 2) == 0); // strict ordering

    rsx::Rsx* rsx = f.kernel->rsx();
    REQUIRE(rsx != nullptr);
    REQUIRE(g.call("cellGcmSetDisplayBuffer", 0, 0x00100000, 5120, 1280, 720) == 0);
    const u32 ctx = ps_read32(b, kCtxPtr);
    const u32 control = static_cast<u32>(g.call("cellGcmGetControlRegister"));
    auto run = [&](auto&& write) {
        u32 cur = ps_read32(b, ctx + 0x8);
        write(cur);
        ps_write32(b, ctx + 0x8, cur);
        store32_release(b, control, cur - kIo); // put, after a sync as the game's (D-585)
        rsx->notify();
        REQUIRE(rsx->wait_idle(2 * kSecond));
    };
    auto user_command = [&](u32& cur, u32 cause) {
        ps_write32(b, cur, rsx::cmd::method_header(rsx::m::GCM_SET_USER_COMMAND, 1));
        ps_write32(b, cur + 4, cause);
        cur += 8;
    };

    // Not set yet: a flip and a user command call nothing.
    run([&](u32& cur) { user_command(cur, 0x11); });
    g.call("_cellGcmSetFlipCommand", ctx, 0);
    run([](u32&) {});
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(ps_read32(b, kFlipCounter) == 0);
    CHECK(ps_read32(b, kUserCounter) == 0);

    g.call("cellGcmSetFlipHandler", kFlipOpd);
    g.call("cellGcmSetUserHandler", kUserOpd);
    run([&](u32& cur) { user_command(cur, 0x55); });
    g.call("_cellGcmSetFlipCommand", ctx, 1);
    run([](u32&) {});
    const auto wait_for = [](auto&& done) {
        for (int i = 0; i < 200 && !done(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return done();
    };
    REQUIRE(wait_for([&] { return ps_read32(b, kUserCounter) == 1 && ps_read32(b, kFlipCounter) == 1; }));
    CHECK(ps_read32(b, kUserCounter + 4) == 0x55); // the user command's argument
    CHECK(ps_read32(b, kFlipCounter + 4) == 1);    // head 1, as RPCS3
}
