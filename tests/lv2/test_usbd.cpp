// sys_usbd with no USB device attached (D-522): the handle, drivers by name, no devices or pipes, the library's own
// events, memory for transfers, and finalisation waking a waiting thread with the termination event.

#include <atomic>
#include <thread>

#include "lv2_test_util.hpp"

using namespace pspocket;
using namespace pspocket::lv2;
using namespace pspocket::lv2::test;

namespace {

constexpr u32 kInitialize = 530, kFinalize = 531, kGetDeviceList = 532, kGetDescriptorSize = 533,
              kGetDescriptor = 534, kRegisterLdd = 535, kUnregisterLdd = 536, kOpenPipe = 537, kOpenDefaultPipe = 538,
              kClosePipe = 539, kReceiveEvent = 540, kDetectEvent = 541, kAttach = 542, kTransferData = 543,
              kGetDeviceLocation = 547, kSendEvent = 548, kEventPortSend = 549, kAllocateMemory = 550,
              kFreeMemory = 551, kGetDeviceSpeed = 556, kUnregisterExtraLdd = 558, kRegisterExtraLdd = 559;

} // namespace

TEST_CASE("sys_usbd: a console with no USB device", "[lv2][usbd]")
{
    Fixture f;
    Caller c(*f.kernel, "usb");
    const u32 out = f.alloc(8);
    const u32 ev = f.alloc(24);

    // Before sys_usbd_initialize.
    CHECK(c.call(kGetDeviceList, 0, out, 127) == rc(CELL_EINVAL));
    CHECK(c.call(kDetectEvent) == rc(CELL_EINVAL));
    CHECK(c.call(kAllocateMemory, 0, out, 0x100) == rc(CELL_EINVAL));
    CHECK(c.call(kReceiveEvent, 0, ev, ev + 8, ev + 16) == rc(CELL_EINVAL));

    REQUIRE(c.call(kInitialize, out) == CELL_OK);
    const u32 handle = f.r32(out);
    CHECK(handle == 0x115B);
    CHECK(c.call(kGetDeviceList, handle, f.alloc(4 * 127), 127) == 0);
    CHECK(c.call(kDetectEvent) == CELL_OK);

    // Drivers by name: a second registration of one is EEXIST, an unknown one ESRCH to unregister.
    const u32 name = f.cstr("testldd");
    CHECK(c.call(kRegisterExtraLdd, handle, name, 7, 0x54C, 0x100, 0x200) == CELL_OK);
    CHECK(c.call(kRegisterExtraLdd, handle, name, 7, 0x54C, 0x100, 0x200) == rc(CELL_EEXIST));
    CHECK(c.call(kUnregisterExtraLdd, handle, name, 7) == CELL_OK);
    CHECK(c.call(kUnregisterExtraLdd, handle, name, 7) == rc(CELL_ESRCH));
    CHECK(c.call(kRegisterLdd, handle, f.cstr("cellUsbPspcm"), 12) == CELL_OK);
    CHECK(c.call(kRegisterLdd, handle, name, 7) == CELL_OK);
    CHECK(c.call(kUnregisterLdd, handle, name, 7) == CELL_OK);

    // No device, so no descriptor, pipe, location, speed, reset or attachment.
    for (const u32 dev : {0u, 1u, 0x10001u}) {
        CHECK(c.call(kGetDescriptorSize, handle, dev) == rc(CELL_EINVAL));
        CHECK(c.call(kGetDescriptor, handle, dev, f.alloc(64), 64) == rc(CELL_EINVAL));
        CHECK(c.call(kOpenPipe, handle, dev, 0, 0, 0, 0x81, 3) == rc(CELL_EINVAL));
        CHECK(c.call(kOpenDefaultPipe, handle, dev) == rc(CELL_EINVAL));
        CHECK(c.call(kClosePipe, handle, dev) == rc(CELL_EINVAL));
        CHECK(c.call(kTransferData, handle, dev, 0, 0, 0, 0) == rc(CELL_EINVAL));
        CHECK(c.call(kGetDeviceLocation, handle, dev, out) == rc(CELL_EINVAL));
        CHECK(c.call(kGetDeviceSpeed, handle, dev, out) == rc(CELL_EINVAL));
        CHECK(c.call(kSendEvent, handle, dev) == rc(CELL_EINVAL));
        CHECK(c.call(kAttach, handle, 0, dev >> 16, dev & 0xFFFF) == rc(CELL_EINVAL));
    }

    // The library's own events come back in order, without waiting.
    CHECK(c.call(kEventPortSend, handle, 1, 2, 3) == CELL_OK);
    CHECK(c.call(kEventPortSend, handle, 5, 6, 7) == CELL_OK);
    REQUIRE(c.call(kReceiveEvent, handle, ev, ev + 8, ev + 16) == CELL_OK);
    CHECK(f.r64(ev) == 1);
    CHECK(f.r64(ev + 16) == 3);
    REQUIRE(c.call(kReceiveEvent, handle, ev, ev + 8, ev + 16) == CELL_OK);
    CHECK(f.r64(ev + 8) == 6);

    // Memory for transfers: 64 KB pages of user memory, until freed.
    CHECK(c.call(kAllocateMemory, handle, out, 0) == rc(CELL_EINVAL));
    REQUIRE(c.call(kAllocateMemory, handle, out, 0x100) == CELL_OK);
    const u32 a = f.r32(out);
    CHECK(a != 0);
    CHECK(a % 0x10000 == 0);
    CHECK(f.mem.is_committed(a));
    CHECK(f.mem.is_committed(a + 0xFFFF));
    f.w32(a + 0x1000, 0x12345678);
    CHECK(c.call(kFreeMemory, handle, a) == CELL_OK);
    CHECK(!f.mem.is_committed(a));
    CHECK(c.call(kFreeMemory, handle, a) == rc(CELL_EINVAL));
    CHECK(c.call(kFreeMemory, handle, 0x10000) == rc(CELL_EINVAL));
    REQUIRE(c.call(kAllocateMemory, handle, out, 0x18000) == CELL_OK);
    const u32 kept = f.r32(out);
    CHECK(f.mem.is_committed(kept + 0x1FFFF));

    // A waiting thread: the library's event, then finalisation's termination (4, 0, 0).
    std::atomic<PpuThread*> waiter{nullptr};
    std::atomic<int> received{0};
    std::thread t([&] {
        Caller me(*f.kernel, "callback");
        const u32 e = f.alloc(24);
        waiter = me.thread;
        THREAD_REQUIRE(me.call(kReceiveEvent, handle, e, e + 8, e + 16) == CELL_OK);
        THREAD_CHECK(f.r64(e) == 2);
        received = 1;
        THREAD_REQUIRE(me.call(kReceiveEvent, handle, e, e + 8, e + 16) == CELL_OK);
        THREAD_CHECK(f.r64(e) == 4);
        THREAD_CHECK(f.r64(e + 8) == 0);
        received = 2;
    });
    const auto waiting = [&] {
        PpuThread* w = waiter.load();
        return w && w->waiter.waiting_on().kind == WaitKind::Usbd;
    };
    REQUIRE(eventually(waiting));
    CHECK(c.call(kEventPortSend, handle, 2, 0x10001, 0) == CELL_OK);
    REQUIRE(eventually([&] { return received.load() == 1 && waiting(); }));
    CHECK(c.call(kFinalize, handle) == CELL_OK);
    t.join();
    CHECK(received == 2);

    // Finalisation freed the memory and ended the handle.
    CHECK(!f.mem.is_committed(kept));
    CHECK(c.call(kFreeMemory, handle, kept) == rc(CELL_EINVAL));
    CHECK(c.call(kGetDeviceList, handle, out, 127) == rc(CELL_EINVAL));
    CHECK(c.call(kEventPortSend, handle, 1, 2, 3) == rc(CELL_EINVAL));
}
