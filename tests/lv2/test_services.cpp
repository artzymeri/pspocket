// lv2's HID manager, sys_config service registry and raw storage (D-527).

#include <cstring>

#include "lv2_test_util.hpp"

using namespace pspocket;
using namespace pspocket::lv2;
using namespace pspocket::lv2::test;

namespace {

constexpr u32 kHidOpen = 500, kHidClose = 501, kHidRead = 502, kHidIoctl = 503, kHidCheckFocus = 510,
              kHidIsRoot = 512, kHid514 = 514;
constexpr u32 kCfgOpen = 516, kCfgClose = 517, kCfgGetEvent = 518, kCfgAddListener = 519, kCfgRemoveListener = 520,
              kCfgRegister = 521, kCfgUnregister = 522, kCfgGetIoEvent = 523;
constexpr u32 kStOpen = 600, kStClose = 601, kStRead = 602, kStWrite = 603, kStAsyncConfigure = 605,
              kStDeviceInfo = 609, kStDeviceConfig = 610, kStReport = 611, kStAsyncCommand = 619, kStSpeed = 623;

Config root_config()
{
    Config c;
    c.root = true;
    return c;
}

u32 make_queue(Fixture& f, Caller& c, s32 size)
{
    const u32 out = f.alloc(4);
    REQUIRE(c.call(kQueueCreate, out, queue_attr(f, SYS_SYNC_FIFO), 0, size) == CELL_OK);
    return f.r32(out);
}

struct Ev {
    u64 source, d1, d2, d3;
};

std::vector<Ev> take(Fixture& f, Caller& c, u32 q)
{
    const u32 events = f.alloc(32 * 8), number = f.alloc(4);
    REQUIRE(c.call(kQueueTryReceive, q, events, 8, number) == CELL_OK);
    std::vector<Ev> out;
    for (u32 i = 0; i < f.r32(number); i++) {
        out.push_back({f.r64(events + 32 * i), f.r64(events + 32 * i + 8), f.r64(events + 32 * i + 16),
            f.r64(events + 32 * i + 24)});
    }
    return out;
}

} // namespace

TEST_CASE("lv2 HID manager: handles, the controller's descriptor, root-only calls", "[lv2][services]")
{
    Fixture f;
    Caller c(*f.kernel, "hid");
    const u32 h = f.alloc(4), buf = f.alloc(64);
    CHECK(c.call(kHidOpen, 4, 0, h) == rc(CELL_EINVAL));
    CHECK(c.call(kHidOpen, 1, 0, 0) == rc(CELL_EFAULT));
    REQUIRE(c.call(kHidOpen, 1, 0, h) == CELL_OK);
    CHECK(f.r32(h) == 0x100);
    REQUIRE(c.call(kHidOpen, 2, 0, h) == CELL_OK);
    CHECK(f.r32(h) == 0x101);
    CHECK(c.call(kHidClose, 0x100) == CELL_OK);

    std::memset(f.base() + buf, 0, 64);
    REQUIRE(c.call(kHidIoctl, 0x101, 2, buf, 21) == CELL_OK);
    CHECK(f.r16(buf) == 0x054C);
    CHECK(f.r16(buf + 2) == 0x0268);
    CHECK(f.base()[buf + 4] == 0x01);
    CHECK(f.base()[buf + 20] == 0x1F);
    REQUIRE(c.call(kHidIoctl, 0x101, 5, buf, 4) == CELL_OK);
    CHECK(f.base()[buf] == 0x4C);
    CHECK(f.base()[buf + 1] == 0x05);
    CHECK(c.call(kHidIoctl, 0x101, 0x68, buf, 64) == CELL_OK);
    CHECK(c.call(kHidCheckFocus) == 1);
    CHECK(c.call(kHid514, 0xE, buf, 1) == CELL_OK);
    CHECK(c.call(kHidRead, 0x100, 2, buf, 64) == rc(CELL_ENOSYS));
    CHECK(c.call(kHidIsRoot, 1) == rc(CELL_ENOSYS));
    for (const u32 n : {504u, 505u, 506u, 507u, 508u, 509u, 511u}) CHECK(c.call(n, 0, 0) == rc(CELL_ENOSYS));

    Fixture r(root_config());
    Caller rc_(*r.kernel, "root");
    const u32 rb = r.alloc(64);
    CHECK(rc_.call(kHidIsRoot, 1) == 1);
    CHECK(rc_.call(kHidRead, 0x100, 2, 0, 64) == rc(CELL_EFAULT));
    CHECK(rc_.call(kHidRead, 0x100, 2, rb, 64) == CELL_OK);
    for (const u32 n : {504u, 505u, 506u, 507u, 508u, 509u, 511u}) CHECK(rc_.call(n, 0, 0) == CELL_OK);
}

TEST_CASE("lv2 sys_config: the pad manager, services and their listeners' events", "[lv2][services]")
{
    Fixture f;
    Caller c(*f.kernel, "config");
    const u32 q = make_queue(f, c, 16);
    const u32 out = f.alloc(4), data = f.alloc(64), ev = f.alloc(256);
    CHECK(c.call(kCfgOpen, 0x8d00ff00, out) == rc(CELL_ESRCH));
    REQUIRE(c.call(kCfgOpen, q, out) == CELL_OK);
    const u32 h = f.r32(out);
    CHECK(h == 0x41000000);

    // The pad manager (0x11) is heard by a listener whose data starts with 1; one without data hears nothing of it.
    f.base()[data] = 0;
    REQUIRE(c.call(kCfgAddListener, h, 0x11, 0, data, 1, 1, out) == CELL_OK);
    CHECK(take(f, c, q).empty());
    f.base()[data] = 1;
    REQUIRE(c.call(kCfgAddListener, h, 0x11, 0, data, 1, 1, out) == CELL_OK);
    const u32 pad_listener = f.r32(out);
    auto got = take(f, c, q);
    REQUIRE(got.size() == 1);
    CHECK(got[0].source == 1);
    CHECK(got[0].d1 == h);
    CHECK(got[0].d2 >> 32 == 1);
    CHECK(got[0].d3 == 47 + 26);
    const u32 pad_event = static_cast<u32>(got[0].d2);
    CHECK(c.call(kCfgGetEvent, h, pad_event, ev, 47) == rc(CELL_EAGAIN));
    REQUIRE(c.call(kCfgGetEvent, h, pad_event, ev, 256) == CELL_OK);
    CHECK(f.r32(ev) == pad_listener);
    CHECK(f.r32(ev + 4) == 1);
    CHECK(f.r64(ev + 8) == 0x11);
    CHECK(f.r64(ev + 24) == 1);
    CHECK(f.r32(ev + 32) == 26);
    CHECK(f.r16(ev + 40 + 10) == 0x054C);
    CHECK(c.call(kCfgGetEvent, h, 0xFFFF, ev, 256) == rc(CELL_ESRCH));

    // A service registered later reaches its listeners; ONCE listeners hear one event; unregistering is heard too.
    REQUIRE(c.call(kCfgAddListener, h, 0x30, 2, 0, 0, 0, out) == CELL_OK); // ONCE, min verbosity 2
    const u32 once = f.r32(out);
    REQUIRE(c.call(kCfgAddListener, h, 0x30, 5, 0, 0, 1, out) == CELL_OK); // repeating, min verbosity 5
    CHECK(take(f, c, q).empty());
    std::memcpy(f.base() + data, "hello", 5);
    REQUIRE(c.call(kCfgRegister, h, 0x30, 7, 3, data, 5, out) == CELL_OK);
    const u32 service = f.r32(out);
    CHECK((service & 0xFF000000) == 0x43000000);
    got = take(f, c, q);
    REQUIRE(got.size() == 1); // only the ONCE listener's verbosity matches
    REQUIRE(c.call(kCfgGetEvent, h, static_cast<u32>(got[0].d2), ev, 256) == CELL_OK);
    CHECK(f.r32(ev) == once);
    CHECK(f.r64(ev + 16) == 7);
    CHECK(std::memcmp(f.base() + ev + 40, "hello", 5) == 0);
    REQUIRE(c.call(kCfgRegister, h, 0x30, 8, 9, data, 5, out) == CELL_OK);
    got = take(f, c, q);
    REQUIRE(got.size() == 1); // the ONCE listener is done; the verbose one hears this one
    REQUIRE(c.call(kCfgGetEvent, h, static_cast<u32>(got[0].d2), ev, 256) == CELL_OK);
    CHECK(f.r64(ev + 16) == 8);
    CHECK(c.call(kCfgUnregister, h, 0x43FFFF00) == rc(CELL_ESRCH));
    REQUIRE(c.call(kCfgUnregister, h, service) == CELL_OK);
    CHECK(take(f, c, q).empty()); // verbosity 3 is below the remaining listener's 5
    CHECK(c.call(kCfgRemoveListener, h, once) == CELL_OK);
    CHECK(c.call(kCfgRemoveListener, h, once) == rc(CELL_ESRCH));
    CHECK(c.call(kCfgGetIoEvent, h, 0, ev, 16) == CELL_OK);
    CHECK(c.call(kCfgClose, h) == CELL_OK);
    CHECK(c.call(kCfgClose, h) == rc(CELL_ESRCH));
    CHECK(c.call(kCfgGetEvent, h, pad_event, ev, 256) == rc(CELL_ESRCH));
}

TEST_CASE("lv2 raw storage: root processes only, devices without contents", "[lv2][services]")
{
    {
        Fixture f;
        Caller c(*f.kernel, "game");
        const u32 out = f.alloc(8);
        CHECK(c.call(kStOpen, 0x101000000000007, 0, out, 0) == rc(CELL_ENOSYS));
        CHECK(c.call(kStDeviceInfo, 0x101000000000007, f.alloc(64)) == rc(CELL_ENOSYS));
        CHECK(c.call(kStDeviceConfig, out, out + 4) == rc(CELL_ENOSYS));
        CHECK(c.call(kStReport, 6, 0, 17, f.alloc(17 * 8)) == rc(CELL_ENOSYS));
        CHECK(c.call(kStRead, 0x45000000, 0, 0, 1, f.alloc(0x200), out, 0) == rc(CELL_ESRCH));
        CHECK(c.call(kStSpeed) == rc(CELL_ENOSYS));
    }
    Fixture f(root_config());
    Caller c(*f.kernel, "root");
    const u32 out = f.alloc(8), buf = f.alloc(0x400), info = f.alloc(64), ids = f.alloc(17 * 8);
    CHECK(c.call(kStOpen, 0, 0, out, 0) == rc(CELL_ENOENT));
    REQUIRE(c.call(kStOpen, 0x101000000000007, 0, out, 0) == CELL_OK);
    const u32 fd = f.r32(out);
    CHECK(fd == 0x45000000);
    std::memset(f.base() + buf, 0x77, 0x400);
    REQUIRE(c.call(kStRead, fd, 0, 0, 2, buf, out, 0) == CELL_OK);
    CHECK(f.r32(out) == 2);
    CHECK(f.base()[buf] == 0);
    CHECK(f.base()[buf + 0x3FF] == 0);
    REQUIRE(c.call(kStWrite, fd, 0, 0, 3, buf, out, 0) == CELL_OK);
    CHECK(f.r32(out) == 3);
    CHECK(c.call(kStClose, fd) == CELL_OK);
    CHECK(c.call(kStWrite, fd, 0, 0, 3, buf, out, 0) == rc(CELL_ESRCH));

    REQUIRE(c.call(kStDeviceInfo, 0x101000000000007ull | (1ull << 32), info) == CELL_OK);
    CHECK(std::memcmp(f.base() + info, "unnamed", 7) == 0);
    CHECK(f.r64(info + 0x28) == 0x24FAEA98);
    CHECK(f.r32(info + 0x30) == 0x200);
    CHECK(c.call(kStDeviceInfo, 0x101000000000007ull | (3ull << 32), info) == -5);
    REQUIRE(c.call(kStDeviceInfo, 0x101000000000006, info) == CELL_OK);
    CHECK(f.r32(info + 0x30) == 0x800);
    REQUIRE(c.call(kStDeviceConfig, out, out + 4) == CELL_OK);
    CHECK(f.r32(out) == 6);
    CHECK(f.r32(out + 4) == 17);
    REQUIRE(c.call(kStReport, 6, 0, 17, ids) == CELL_OK);
    CHECK(f.r64(ids) == 0x10300000000000A);
    CHECK(f.r64(ids + 8 * 11) == 0x101000000000006);
    CHECK(f.r64(ids + 8 * 16) == 0x100000000000003);
    CHECK(c.call(kStReport, 6, 10, 8, ids) == rc(CELL_EINVAL));

    const u32 q = make_queue(f, c, 8);
    CHECK(c.call(kStAsyncConfigure, fd, 0, 0x8d00ff00, 0) == rc(CELL_ESRCH));
    REQUIRE(c.call(kStAsyncConfigure, fd, 0, q, 0) == CELL_OK);
    REQUIRE(c.call(kStAsyncCommand, fd, 1, 0, 0, 0, 0, 0x55) == CELL_OK);
    const auto got = take(f, c, q);
    REQUIRE(got.size() == 1);
    CHECK(got[0].source == 0);
    CHECK(got[0].d1 == 0x55);
}
