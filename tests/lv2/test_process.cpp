// sys_process and sys_tty through the dispatcher, and process exit tearing down every guest thread.

#include "lv2_test_util.hpp"

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
