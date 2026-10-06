// cellCamera and cellMic with no device attached, as RPCS3's default (null) handlers answer (synthetic: no game data).

#include "hle_test_util.hpp"

#include <cstring>

#include "pspocket/lv2/sync.hpp"
#include "pspocket/lv2/syscalls.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kInfo = kData + 0x100, kOut = kData + 0x200, kAttr = kData + 0x300, kQueue = kData + 0x400;
constexpr u32 kUnmapped = 0x0FFF0000; // in the main area, never committed by these tests

} // namespace

TEST_CASE("cellCamera: initialised with no camera attached", "[hle][devices]")
{
    Fixture f;
    GuestThread g(f, 0, "camera");
    u8* b = f.base();
    const u64 kAlready = r3_of_code(0x80140801), kNotInit = r3_of_code(0x80140803), kParam = r3_of_code(0x80140804),
              kNotOpen = r3_of_code(0x80140806), kNotFound = r3_of_code(0x80140807),
              kFormat = r3_of_code(0x8014080A), kResolution = r3_of_code(0x8014080B),
              kFramerate = r3_of_code(0x8014080C);

    CHECK(g.call("cellCameraOpen", 0, kInfo) == kNotInit);
    CHECK(g.call("cellCameraEnd") == kNotInit);
    REQUIRE(g.call("cellCameraInit") == 0);
    CHECK(g.call("cellCameraInit") == kAlready);

    // Open checks its arguments (CellCameraInfo {format, resolution, framerate, ...}), then finds no camera.
    std::memset(b + kInfo, 0, 64);
    CHECK(g.call("cellCameraOpen", 0, 0) == kParam);
    CHECK(g.call("cellCameraOpen", 1, kInfo) == kParam);
    CHECK(g.call("cellCameraOpen", 0, kInfo) == kFormat);
    ps_write32(b, kInfo, 3); // YUV422
    CHECK(g.call("cellCameraOpen", 0, kInfo) == kResolution);
    ps_write32(b, kInfo + 4, 1); // VGA
    ps_write32(b, kInfo + 8, 24);
    CHECK(g.call("cellCameraOpen", 0, kInfo) == kFramerate);
    ps_write32(b, kInfo + 8, 30);
    CHECK(g.call("cellCameraOpen", 0, kInfo) == kNotFound);
    CHECK(g.call("cellCameraOpen", 0, kUnmapped) == kParam);
    CHECK(g.call("cellCameraOpenEx", 0, kInfo) == kNotFound);
    CHECK(g.call("cellCameraOpenEx", 0, 0) == kParam);

    CHECK(g.call("cellCameraIsAvailable", 0) == 0);
    CHECK(g.call("cellCameraIsAttached", 0) == 0);
    CHECK(g.call("cellCameraIsOpen", 0) == 0);
    CHECK(g.call("cellCameraIsStarted", 0) == 0);
    CHECK(g.call("cellCameraGetType", 0, kOut) == kNotFound);
    CHECK(g.call("cellCameraGetBufferSize", 0, kInfo) == kNotFound);
    ps_write32(b, kOut, 0xAAAA);
    REQUIRE(g.call("cellCameraGetDeviceGUID", 0, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0);

    for (const char* fn : {"cellCameraClose", "cellCameraReset", "cellCameraStart", "cellCameraStop"}) {
        INFO(fn);
        CHECK(g.call(fn, 0) == kNotOpen);
        CHECK(g.call(fn, 1) == kParam);
    }
    CHECK(g.call("cellCameraGetAttribute", 0, 0, kOut, kAttr) == kNotOpen);
    CHECK(g.call("cellCameraGetAttribute", 1, 0, kOut, kAttr) == kParam);
    CHECK(g.call("cellCameraSetAttribute", 0, 0, 1, 0) == kNotOpen);
    CHECK(g.call("cellCameraGetBufferInfoEx", 0, kInfo) == kNotOpen);
    CHECK(g.call("cellCameraGetBufferInfo", 0, kInfo) == kNotOpen);
    CHECK(g.call("cellCameraReadEx", 0, kOut) == kNotOpen);
    CHECK(g.call("cellCameraRead", 0, kOut, kAttr) == kNotOpen);
    CHECK(g.call("cellCameraReadComplete", 0, 0, 0) == kNotOpen);
    CHECK(g.call("cellCameraPrepExtensionUnit", 0, kOut) == kNotFound);
    CHECK(g.call("cellCameraPrepExtensionUnit", 0, 0) == kParam);
    CHECK(g.call("cellCameraGetExtensionUnit", 0, 0, 4, kOut) == kNotOpen);
    CHECK(g.call("cellCameraSetExtensionUnit", 1, 0, 4, kOut) == kParam);
    CHECK(g.call("cellCameraSetNotifyEventQueue", 0x1234) == 0);
    CHECK(g.call("cellCameraSetNotifyEventQueue2", 0x1234, 0, 0) == 0);
    CHECK(g.call("cellCameraRemoveNotifyEventQueue", 0x1234) == 0);

    REQUIRE(g.call("cellCameraEnd") == 0);
    CHECK(g.call("cellCameraSetNotifyEventQueue", 0x1234) == kNotInit);
    CHECK(g.call("cellCameraGetAttribute", 0, 0, kOut, kAttr) == kNotInit);
}

TEST_CASE("cellMic: initialised with no microphone registered", "[hle][devices]")
{
    Fixture f;
    GuestThread g(f, 0, "mic");
    u8* b = f.base();
    const u64 kAlready = r3_of_code(0x80140101), kNotInit = r3_of_code(0x80140103), kParam = r3_of_code(0x80140104),
              kQueueError = r3_of_code(0x8014010D), kNotFound = r3_of_code(0x8014010E);

    CHECK(g.call("cellMicOpen", 0, 48000) == kNotInit);
    REQUIRE(g.call("cellMicInit") == 0);
    CHECK(g.call("cellMicInit") == kAlready);

    for (const char* fn : {"cellMicClose", "cellMicStart", "cellMicStop", "cellMicReset"}) {
        INFO(fn);
        CHECK(g.call(fn, 0) == kNotFound);
    }
    CHECK(g.call("cellMicOpen", 0, 48000) == kNotFound);
    CHECK(g.call("cellMicOpenRaw", 0, 48000, 2) == kNotFound);
    CHECK(g.call("cellMicOpenEx", 0, 48000, 1, 48000, 128, 1) == kNotFound);
    CHECK(g.call("cellMicStartEx", 0, 4) == kParam);
    CHECK(g.call("cellMicStartEx", 0, 3) == kNotFound);
    CHECK(g.call("cellMicRead", 0, kOut, 64) == kNotFound);
    CHECK(g.call("cellMicReadRaw", 0, kOut, 64) == kNotFound);
    CHECK(g.call("cellMicGetDeviceAttr", static_cast<u64>(-1), 9, kOut, 0) == kParam);
    CHECK(g.call("cellMicGetDeviceAttr", 0, 9, kOut, 0) == kNotFound);
    CHECK(g.call("cellMicSetDeviceAttr", 0, 9, 1, 0) == kNotFound);
    CHECK(g.call("cellMicGetSignalState", 0, 0, 0) == kParam);
    CHECK(g.call("cellMicGetSignalState", 0, 0, kOut) == kNotFound);
    CHECK(g.call("cellMicGetFormat", 0, 0) == kParam);
    CHECK(g.call("cellMicGetFormatRaw", 0, kOut) == kNotFound);
    CHECK(g.call("cellMicIsOpen", 0) == 0);
    CHECK(g.call("cellMicIsAttached", 0) == 1); // RPCS3: always

    // Type and status: RPCS3's defaults for the 8 device slots.
    REQUIRE(g.call("cellMicGetType", 0, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 3); // CELLMIC_TYPE_USBAUDIO
    REQUIRE(g.call("cellMicGetType", 8, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0xFFFFFFFF); // CELLMIC_TYPE_UNDEF
    CHECK(g.call("cellMicGetType", 0, 0) == kParam);
    REQUIRE(g.call("cellMicGetStatus", 1, kOut) == 0);
    CHECK(ps_read32(b, kOut + 0) == 48000);
    CHECK(ps_read32(b, kOut + 16) == 0); // isOpen
    CHECK(ps_readf32(b, kOut + 28) == 60.0f);
    CHECK(g.call("cellMicGetStatus", static_cast<u64>(-1), kOut) == kParam);
    REQUIRE(g.call("cellMicGetDeviceGUID", 0, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0xFFFFFFFF);
    REQUIRE(g.call("cellMicGetDeviceIdentifier", 0, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0);

    // A notify queue must exist; it gets no attach events.
    CHECK(g.call("cellMicSetNotifyEventQueue", 0x1234) == kQueueError);
    ps_write32(b, kAttr, lv2::SYS_SYNC_FIFO);
    ps_write32(b, kAttr + 4, lv2::SYS_PPU_QUEUE);
    ps_write64(b, kAttr + 8, 0);
    REQUIRE(lv2::sys_event_queue_create(*g.t, kQueue, kAttr, 0x8000000000AB0001ull, 8) == 0);
    REQUIRE(g.call("cellMicSetNotifyEventQueue2", 0x8000000000AB0001ull, 7, 0) == 0);
    REQUIRE(lv2::sys_event_queue_tryreceive(*g.t, ps_read32(b, kQueue), kOut, 4, kInfo) == 0);
    CHECK(ps_read32(b, kInfo) == 0);
    REQUIRE(g.call("cellMicRemoveNotifyEventQueue", 0x8000000000AB0001ull) == 0);

    REQUIRE(g.call("cellMicEnd") == 0);
    CHECK(g.call("cellMicEnd") == kNotInit);
    CHECK(g.call("cellMicGetDeviceGUID", 0, kOut) == kNotInit); // written all the same (RPCS3)
    CHECK(ps_read32(b, kOut) == 0xFFFFFFFF);
}
