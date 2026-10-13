// sys_process and sys_tty through the dispatcher, and process exit tearing down every guest thread.

#include "lv2_test_util.hpp"

#include <cstring>

using namespace pspocket;
using namespace pspocket::lv2;
using namespace pspocket::lv2::test;

TEST_CASE("lv2 process: pid, SDK version, PARAM.SFO title and object counts", "[lv2][process]")
{
    Config config;
    config.sdk_version = 0x00470001;
    Fixture f(config);
    Caller c(*f.kernel);
    CHECK(c.call(kGetPid) == 1);
    const u32 v = f.alloc(4);
    REQUIRE(c.call(kGetSdkVersion, 1, v) == CELL_OK);
    CHECK(f.r32(v) == 0x00470001);
    CHECK(c.call(kGetSdkVersion, 1, 0) == rc(CELL_EFAULT));
    CHECK(c.call(kGetParamSfo, f.alloc(0x40)) == rc(CELL_ENOENT)); // no title configured

    const u32 out = f.alloc(4);
    for (int i = 0; i < 2; i++) REQUIRE(c.call(kMutexCreate, out, mutex_attr(f, SYS_SYNC_FIFO, SYS_SYNC_NOT_RECURSIVE)) == CELL_OK);
    REQUIRE(c.call(kSemCreate, out, sync_attr(f, SYS_SYNC_FIFO), 0, 1) == CELL_OK);
    const u32 n = f.alloc(4);
    REQUIRE(c.call(kGetNumberOfObject, 0x85, n) == CELL_OK);
    CHECK(f.r32(n) == 2);
    REQUIRE(c.call(kGetNumberOfObject, 0x96, n) == CELL_OK);
    CHECK(f.r32(n) == 1);
    REQUIRE(c.call(kGetNumberOfObject, 0x86, n) == CELL_OK);
    CHECK(f.r32(n) == 0);
    CHECK(c.call(kGetNumberOfObject, 0x99, n) == rc(CELL_EINVAL));
}

TEST_CASE("lv2 process: _sys_process_get_paramsfo with a title", "[lv2][process]")
{
    Config config;
    config.title_id = "BLES00229";
    Fixture f(config);
    Caller c(*f.kernel);
    const u32 buf = f.alloc(0x40);
    std::memset(f.base() + buf, 0xAA, 0x40);
    REQUIRE(c.call(kGetParamSfo, buf) == CELL_OK);
    CHECK(f.base()[buf] == 0);
    CHECK(std::string(reinterpret_cast<const char*>(f.base() + buf + 1), 9) == "BLES00229");
    for (u32 i = 10; i < 0x40; i++) CHECK(f.base()[buf + i] == 0);
}

// D-943: an executable that declares SYS_PROCESS_CRASH_DUMP_ENABLED (sys_process_param's crash_dump_param_addr) is
// marked with flag 0x10 at +0x10 and the declaration's address at +0x38, as the retail kernel does; +0x20, a debugging
// console's crash dump settings, stays 0.
TEST_CASE("lv2 process: _sys_process_get_paramsfo reports a crash dump declaration", "[lv2][process]")
{
    Config config;
    config.title_id = "BLES00229";
    config.crash_dump_param_addr = 0x00012340;
    Fixture f(config);
    Caller c(*f.kernel);
    const u32 buf = f.alloc(0x40);
    std::memset(f.base() + buf, 0xAA, 0x40);
    REQUIRE(c.call(kGetParamSfo, buf) == CELL_OK);
    CHECK(std::string(reinterpret_cast<const char*>(f.base() + buf + 1), 9) == "BLES00229");
    CHECK(f.r64(buf + 0x10) == 0x10);
    CHECK(f.r64(buf + 0x18) == 0);
    CHECK(f.r64(buf + 0x20) == 0);
    CHECK(f.r64(buf + 0x28) == 0);
    CHECK(f.r64(buf + 0x30) == 0);
    CHECK(f.r64(buf + 0x38) == 0x00012340);
}

TEST_CASE("lv2 tty: writes go to the tty log channel", "[lv2][tty]")
{
    Fixture f;
    Caller c(*f.kernel);
    LogCapture log;
    const std::string_view text = "first line\nsecond line\n";
    const u32 written = f.alloc(4);
    REQUIRE(c.call(kTtyWrite, 0, f.cstr(text), text.size(), written) == CELL_OK);
    CHECK(f.r32(written) == text.size());
    CHECK(log.contains("tty", "first line"));
    CHECK(log.contains("tty", "second line"));
    REQUIRE(c.call(kTtyWrite, 3, f.cstr("user channel"), 12, written) == CELL_OK);
    CHECK(log.contains("tty", "[3] user channel"));
    CHECK(c.call(kTtyWrite, 16, f.cstr("x"), 1, written) == rc(CELL_EINVAL));
    CHECK(c.call(kTtyWrite, 0, 0x70000000, 4, written) == rc(CELL_EFAULT));
    CHECK(c.call(kTtyWrite, 0, f.cstr("x"), 1, 0) == rc(CELL_EFAULT));
    CHECK(c.call(kTtyRead, 0, f.alloc(16), 16, f.alloc(4)) == rc(CELL_EIO));
}

TEST_CASE("lv2 process: exit unwinds every guest thread", "[lv2][process]")
{
    HostEntries entries;
    std::atomic<s32> exit_status{-1};
    Config config = entries.config();
    config.on_process_exit = [&](s32 status) { exit_status = status; };
    Fixture f(config);
    Caller main(*f.kernel, "main");

    const u32 out = f.alloc(4);
    REQUIRE(main.call(kQueueCreate, out, queue_attr(f, SYS_SYNC_FIFO), 0, 8) == CELL_OK);
    const u32 q = f.r32(out);
    std::atomic<bool> blocked_returned{false};
    entries.add(0x1000, [&](PPUContext& ctx) {
        syscall(ctx, kQueueReceive, q, 0, 0); // blocks until the process exits, then unwinds
        blocked_returned = true;
    });
    entries.add(0x2000, [&](PPUContext& ctx) {
        syscall(ctx, kUsleep, 10000);
        syscall(ctx, kProcessExit3, 5);
        blocked_returned = true; // never reached
    });
    auto spawn = [&](u32 entry) {
        const u32 opd = f.alloc(8);
        f.w32(opd, entry);
        const u32 param = f.alloc(8);
        f.w32(param, opd);
        const u32 idp = f.alloc(8);
        REQUIRE(main.call(kThreadCreate, idp, param, 0, 0, 1000, 0x4000, 1, 0) == CELL_OK);
        REQUIRE(main.call(kThreadStart, static_cast<u32>(f.r64(idp))) == CELL_OK);
    };
    spawn(0x1000);
    REQUIRE(eventually([&] { return queued<EventQueue>(*f.kernel, q) == 1; }));
    spawn(0x2000);

    REQUIRE(f.kernel->wait_for_exit(std::chrono::milliseconds(5000)));
    CHECK(f.kernel->exit_status() == 5);
    CHECK(exit_status.load() == 5);
    CHECK(f.kernel->shutdown(std::chrono::milliseconds(5000))); // every host thread finished
    CHECK_FALSE(blocked_returned.load());
    // After the exit, syscalls unwind their caller too.
    CHECK_THROWS_AS(main.call(kGetPid), ThreadExit);
}

TEST_CASE("lv2 process: object IDs and lock-line reservation addresses (D-184)", "[lv2][process]")
{
    Fixture f;
    Caller c(*f.kernel);
    const u32 out = f.alloc(4);
    u32 ids[3];
    for (u32& id : ids) {
        REQUIRE(c.call(kMutexCreate, out, mutex_attr(f, SYS_SYNC_FIFO, SYS_SYNC_NOT_RECURSIVE)) == CELL_OK);
        id = f.r32(out);
    }
    const u32 buf = f.alloc(16), set = f.alloc(4);
    REQUIRE(c.call(13, 0x85, buf, 4, set) == CELL_OK); // sys_process_get_id
    CHECK(f.r32(set) == 3);
    CHECK(f.r32(buf) == ids[0]);
    CHECK(f.r32(buf + 8) == ids[2]);
    REQUIRE(c.call(13, 0x85, buf, 0, set) == CELL_OK); // 0 counts as 1 (the firmware's signed do-while)
    CHECK(f.r32(set) == 1);
    REQUIRE(c.call(13, 0x96, buf, 4, set) == CELL_OK); // no semaphores
    CHECK(f.r32(set) == 0);
    CHECK(c.call(13, 0x24, buf, 4, set) == rc(CELL_EINVAL)); // SPU ports: not here
    CHECK(c.call(13, 0x99, buf, 4, set) == rc(CELL_EINVAL));
    CHECK(c.call(29, 0x85, buf, 4, set) == rc(CELL_ENOSYS)); // get_id2: root only

    // sys_process_is_spu_lock_line_reservation_address(addr, flags): by 256 MB segment (RPCS3).
    CHECK(c.call(14, 0x00100000, 2) == CELL_OK);  // main memory, SPU threads
    CHECK(c.call(14, 0xC0000000, 4) == CELL_OK);  // RSX local memory, raw SPU
    CHECK(c.call(14, 0xF0000000, 2) == CELL_OK);  // private SPU MMIO, SPU threads
    CHECK(c.call(14, 0xF0000000, 4) == rc(CELL_EPERM));
    CHECK(c.call(14, 0xD0000000, 2) == rc(CELL_EPERM)); // the PPU stack area
    CHECK(c.call(14, 0x40000000, 2) == rc(CELL_EINVAL)); // nothing mapped there
    CHECK(c.call(14, 0x00100000, 0) == rc(CELL_EINVAL));
    CHECK(c.call(14, 0x00100000, 1) == rc(CELL_EINVAL));
}

TEST_CASE("lv2 sys_game and sys_ss: watchdog, versions, board storage, random numbers, PSID (D-184)",
    "[lv2][process]")
{
    Config config;
    config.system_sw_version = 48800;
    config.console_psid_high = 0x1122334455667788ull;
    config.console_psid_low = 0x99AABBCCDDEEFF00ull;
    Fixture f(config);
    Caller c(*f.kernel);

    REQUIRE(c.call(372, 30) == CELL_OK);              // watchdog start, 30 s
    CHECK(c.call(372, 30) == rc(CELL_EABORT));        // already running
    CHECK(c.call(374) == CELL_OK);                    // clear
    CHECK(c.call(373) == CELL_OK);                    // stop
    CHECK(c.call(372, 30) == CELL_OK);                // and again
    CHECK(c.call64(376) == 48800);                    // the system software version
    CHECK(c.call(375, 49000) == rc(CELL_ENOSYS));     // setting it: root only

    const u32 board = f.alloc(16), status = f.alloc(4);
    REQUIRE(c.call(410, board, status) == CELL_OK);
    CHECK(f.r32(board) == 0xFFFFFFFF); // unwritten: all ones
    CHECK(f.base()[status] == 0);
    for (u32 i = 0; i < 16; i++) f.base()[board + i] = static_cast<u8>(i);
    REQUIRE(c.call(411, board, status) == CELL_OK);
    std::memset(f.base() + board, 0, 16);
    REQUIRE(c.call(410, board, status) == CELL_OK);
    CHECK(f.base()[board + 15] == 15);
    CHECK(c.call(410, 0, status) == rc(CELL_EFAULT));
    CHECK(c.call(412, status) == rc(CELL_ENOSYS)); // _sys_game_get_rtc_status: not in a retail lv2 (D-938)

    // Random numbers: package 2 only for a game; at most 256 MB.
    const u32 rnd = f.alloc(64);
    std::memset(f.base() + rnd, 0, 64);
    REQUIRE(c.call(865, 2, rnd, 64) == CELL_OK);
    bool any = false;
    for (u32 i = 0; i < 64; i++) any |= f.base()[rnd + i] != 0;
    CHECK(any);
    CHECK(c.call(865, 1, rnd, 0x18) == rc(CELL_ENOSYS));
    CHECK(static_cast<u32>(c.call(865, 3, rnd, 8)) == 0x80010500u);
    CHECK(static_cast<u32>(c.call(865, 2, rnd, 0x10000001)) == 0x80010501u);

    const u32 psid = f.alloc(16);
    REQUIRE(c.call(872, psid) == CELL_OK);
    CHECK(f.r64(psid) == 0x1122334455667788ull);
    CHECK(f.r64(psid + 8) == 0x99AABBCCDDEEFF00ull);
    CHECK(c.call(870, psid) == rc(CELL_ENOSYS)); // the console ID: root only
    const u32 value = f.alloc(8);
    REQUIRE(c.call(873, value) == CELL_OK);
    CHECK(f.base()[value] == 0xFF);
    REQUIRE(c.call(874, value) == CELL_OK);
    CHECK(f.r64(value) == 0xFE);
    REQUIRE(c.call(875, value) == CELL_OK);
    CHECK(f.r64(value) == 0x190);
    CHECK(c.call(875, 0) == rc(CELL_EINVAL));
    REQUIRE(c.call(866, 0x3002, 0, value, psid) == CELL_OK); // the secure RTC: the time base, status 0
    CHECK(f.r64(value) > 0);
    CHECK(f.r64(psid) == 0);
    CHECK(static_cast<u32>(c.call(866, 0x3004, 0, 0, 0)) == 0x80010500u);
    REQUIRE(c.call(871, 2, value, 0) == CELL_OK); // the authority ID
    CHECK(c.call(871, 1, 1, value) == rc(CELL_ENOSYS));
    CHECK(static_cast<u32>(c.call(871, 9, 0, 0)) == 0x8001051Du);
    REQUIRE(c.call(868, 0x17001, 0, value, 0, 0, 0) == CELL_OK);
    CHECK(f.r64(value) == 0x100);
}

TEST_CASE("lv2 process: RPCS3's stub syscalls answer its constant (D-184)", "[lv2][process]")
{
    Fixture f;
    Caller c(*f.kernel);
    CHECK(c.call(60, 0, 0) == static_cast<s32>(0x80010003)); // sys_trace_create: CELL_ENOSYS-like constant
    CHECK(c.call(976, 0) == rc(CELL_EFAULT));                 // sys_crypto_engine_create: the ID unwritable (D-937)
    CHECK(c.call(119, 0) == rc(CELL_ENOSYS));                 // sys_time_get_rtc: root's (D-527)
    CHECK(c.call(810, f.cstr("/a"), f.cstr("/b")) == rc(CELL_ENOTMOUNTED)); // sys_fs_link: pspocket's own (D-518)
    CHECK(c.call(213, f.cstr("text"), 4) == rc(CELL_ENOSYS)); // sys_console_write2: not in a retail lv2 (D-938)
    CHECK(c.call(510) == 1);                                  // sys_hid_manager_check_focus
}
