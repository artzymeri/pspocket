// cellCamera, cellMic and libgem with no device attached, as RPCS3's default (null) handlers answer (synthetic: no game data).

#include "hle_test_util.hpp"

#include <chrono>
#include <cstring>
#include <thread>

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

TEST_CASE("libgem: initialised with no Move controller connected", "[hle][devices]")
{
    Fixture f;
    GuestThread g(f, 0, "gem");
    u8* b = f.base();
    const u64 kAlready = r3_of_code(0x80121802), kUninit = r3_of_code(0x80121803), kParam = r3_of_code(0x80121804),
              kAlign = r3_of_code(0x80121805), kUpdateNotFinished = r3_of_code(0x80121806),
              kUpdateNotStarted = r3_of_code(0x80121807), kConvertNotFinished = r3_of_code(0x80121808),
              kConvertNotStarted = r3_of_code(0x80121809), kNotAHue = r3_of_code(0x8012180B);
    constexpr u64 kNotConnected = 1, kNoVideo = 6;
    constexpr u32 kAttrib = kData + 0x500, kVc = kData + 0x600, kFrame = kData + 0x1000, kHues = kData + 0x2000;

    CHECK(g.call("cellGemGetMemorySize", 2) == 0x120000);
    CHECK(g.call("cellGemGetMemorySize", 4) == 0x140000);
    CHECK(g.call("cellGemGetMemorySize", 5) == kParam);
    CHECK(g.call("cellGemGetInfo", kOut) == kUninit);
    CHECK(g.call("cellGemEnd") == kUninit);

    // CellGemAttribute {version, max_connect, memory_ptr, spurs_addr, spu_priorities[8]}: SPURS is required.
    std::memset(b + kAttrib, 0, 24);
    ps_write32(b, kAttrib + 0, 2);
    ps_write32(b, kAttrib + 4, 2);
    CHECK(g.call("cellGemInit", kAttrib) == kParam); // no SPURS
    ps_write32(b, kAttrib + 12, kData + 0x8000);
    ps_write32(b, kAttrib + 4, 5);
    CHECK(g.call("cellGemInit", kAttrib) == kParam);
    ps_write32(b, kAttrib + 4, 2);
    REQUIRE(g.call("cellGemInit", kAttrib) == 0); // allocates its own memory (none was given)
    CHECK(g.call("cellGemInit", kAttrib) == kAlready);

    std::memset(b + kOut, 0xAA, 40);
    REQUIRE(g.call("cellGemGetInfo", kOut) == 0);
    CHECK(ps_read32(b, kOut + 0) == 2); // max_connect
    CHECK(ps_read32(b, kOut + 4) == 0); // now_connect
    CHECK(ps_read32(b, kOut + 8) == 0); // status[0]: disconnected
    CHECK(ps_read32(b, kOut + 36) == 0); // port[3]

    // No controller is ready.
    CHECK(g.call("cellGemGetState", 0, 0, 0, kOut) == kNotConnected);
    CHECK(g.call("cellGemGetState", 0, 3, 0, kOut) == kParam);
    CHECK(g.call("cellGemGetState", 4, 0, 0, kOut) == kParam);
    CHECK(g.call("cellGemGetInertialState", 0, 0, 0, kOut) == kParam);
    CHECK(g.call("cellGemEnableMagnetometer", 0, 1) == kNotConnected);
    CHECK(g.call("cellGemReadExternalPortDeviceInfo", 0, kOut, kHues) == kNotConnected);
    CHECK(g.call("cellGemWriteExternalPort", 1, kHues) == kNotConnected);
    REQUIRE(g.call("cellGemGetImageState", 0, kOut) == 0);
    CHECK(ps_read8(b, kOut + 40) == 0); // visible

    // The sphere colours: RPCS3's defaults, forced colours, tracked hues.
    REQUIRE(g.call("cellGemGetRGB", 1, kOut, kOut + 4, kOut + 8) == 0);
    CHECK(ps_readf32(b, kOut) == 1.0f);
    CHECK(ps_readf32(b, kOut + 4) == 0.85f); // gold
    g.ctx.fpr[1] = 0.0;
    g.ctx.fpr[2] = 0.0;
    g.ctx.fpr[3] = 2.0; // clamped to 1
    REQUIRE(g.call("cellGemForceRGB", 0) == 0);
    REQUIRE(g.call("cellGemGetRGB", 0, kOut, kOut + 4, kOut + 8) == 0);
    CHECK(ps_readf32(b, kOut + 8) == 1.0f);
    CHECK(g.call("cellGemGetTrackerHue", 0, kOut) == kNotAHue); // forcing a colour stops tracking
    ps_write32(b, kHues + 0, 4 << 24); // don't care: blue
    ps_write32(b, kHues + 4, 2 << 24); // don't track
    ps_write32(b, kHues + 8, 60);
    ps_write32(b, kHues + 12, 4 << 24); // don't care: purple
    REQUIRE(g.call("cellGemTrackHues", kHues, kHues + 16) == 0);
    CHECK(ps_read32(b, kHues + 16) == 240);
    CHECK(ps_read32(b, kHues + 20) == (2u << 24));
    CHECK(ps_read32(b, kHues + 24) == 60);
    CHECK(ps_read32(b, kHues + 28) == 300);
    REQUIRE(g.call("cellGemGetTrackerHue", 2, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 60);
    REQUIRE(g.call("cellGemGetRGB", 2, kOut, kOut + 4, kOut + 8) == 0);
    CHECK(ps_readf32(b, kOut) == 1.0f); // hue 60: yellow
    CHECK(ps_readf32(b, kOut + 4) == 1.0f);
    CHECK(ps_readf32(b, kOut + 8) == 0.0f);

    // Calibration completes 0.5 s later.
    REQUIRE(g.call("cellGemCalibrate", 1) == 0);
    CHECK(g.call("cellGemCalibrate", 1) == r3_of(lv2::CELL_EBUSY));
    REQUIRE(g.call("cellGemGetStatusFlags", 1, kOut) == 0);
    CHECK(ps_read64(b, kOut) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(550));
    REQUIRE(g.call("cellGemGetStatusFlags", 1, kOut) == 0);
    CHECK(ps_read64(b, kOut) == 3); // occurred | succeeded
    REQUIRE(g.call("cellGemClearStatusFlags", 1, 1) == 0);
    REQUIRE(g.call("cellGemGetStatusFlags", 1, kOut) == 0);
    CHECK(ps_read64(b, kOut) == 2);
    REQUIRE(g.call("cellGemInvalidateCalibration", 1) == 0);
    REQUIRE(g.call("cellGemGetStatusFlags", 1, kOut) == 0);
    CHECK(ps_read64(b, kOut) == 0);

    REQUIRE(g.call("cellGemSetRumble", 3, 200) == 0);
    REQUIRE(g.call("cellGemGetRumble", 3, kOut) == 0);
    CHECK(ps_read8(b, kOut) == 200);
    REQUIRE(g.call("cellGemReset", 3) == 0);
    REQUIRE(g.call("cellGemGetRumble", 3, kOut) == 0);
    CHECK(ps_read8(b, kOut) == 0);

    // Hues and colours.
    CHECK(g.call("cellGemIsTrackableHue", 100) == 1);
    CHECK(g.call("cellGemIsTrackableHue", 360) == kParam);
    REQUIRE(g.call("cellGemGetAllTrackableHues", kHues) == 0);
    CHECK(ps_read8(b, kHues + 359) == 1);
    g.ctx.fpr[1] = 120.0; // green
    g.ctx.fpr[2] = 1.0;
    g.ctx.fpr[3] = 0.5;
    REQUIRE(g.call("cellGemHSVtoRGB", kOut, kOut + 4, kOut + 8) == 0);
    CHECK(ps_readf32(b, kOut) == 0.0f);
    CHECK(ps_readf32(b, kOut + 4) == 0.5f);
    CHECK(ps_readf32(b, kOut + 8) == 0.0f);
    REQUIRE(g.call("cellGemGetCameraState", kOut) == 0);
    CHECK(ps_readf32(b, kOut + 8) == 1.0f); // gain
    REQUIRE(g.call("cellGemGetEnvironmentLightingColor", kOut, kOut + 4, kOut + 8) == 0);
    CHECK(ps_readf32(b, kOut) == 128.0f);

    // Updates: no camera, no video.
    CHECK(g.call("cellGemUpdateFinish") == kUpdateNotStarted);
    CHECK(g.call("cellGemUpdateStart", kFrame, 0) == kNoVideo);
    CHECK(g.call("cellGemUpdateStart", kFrame, 0) == kUpdateNotFinished);
    CHECK(g.call("cellGemUpdateFinish") == 0); // a frame was given
    CHECK(g.call("cellGemUpdateStart", kFrame + 4, 0) == kAlign); // started all the same (RPCS3)
    CHECK(g.call("cellGemUpdateFinish") == 0);

    // Video conversion: prepared with RPCS3's checks; a conversion writes nothing.
    std::memset(b + kVc, 0, 40);
    ps_write32(b, kVc + 0, 2);  // version
    ps_write32(b, kVc + 4, 2);  // RGBA_640x480
    CHECK(g.call("cellGemPrepareVideoConvert", kVc) == kParam); // no output buffer
    ps_write32(b, kVc + 32, kFrame + 0x80);
    REQUIRE(g.call("cellGemPrepareVideoConvert", kVc) == 0);
    ps_write32(b, kVc + 32, kFrame + 0x40);
    CHECK(g.call("cellGemPrepareVideoConvert", kVc) == kAlign);
    CHECK(g.call("cellGemConvertVideoFinish") == kConvertNotStarted);
    REQUIRE(g.call("cellGemConvertVideoStart", kFrame) == 0);
    CHECK(g.call("cellGemConvertVideoStart", kFrame) == kConvertNotFinished);
    REQUIRE(g.call("cellGemConvertVideoFinish") == 0);

    REQUIRE(g.call("cellGemEnd") == 0);
    CHECK(g.call("cellGemEnd") == kUninit);
}
