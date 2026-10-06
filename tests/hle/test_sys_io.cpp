// sys_io: the virtual controller on port 0 and the absent keyboard (synthetic: no game data).

#include "hle_test_util.hpp"

#include <chrono>
#include <thread>

#include "pspocket/hle/input.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kInfo = kData + 0x100, kPad = kData + 0x200, kAct = kData + 0x300, kKb = kData + 0x400;
constexpr u64 kPadInval = 0xFFFFFFFF80121102ull, kPadAlready = 0xFFFFFFFF80121103ull,
              kPadUninit = 0xFFFFFFFF80121104ull, kPadNoDevice = 0xFFFFFFFF80121107ull;
constexpr u64 kKbFatal = 0xFFFFFFFF80121001ull, kKbInval = 0xFFFFFFFF80121002ull, kKbUninit = 0xFFFFFFFF80121004ull,
              kKbNoDevice = 0xFFFFFFFF80121007ull, kKbSetting = 0xFFFFFFFF80121008ull;

u16 button(u8* b, u32 i) { return ps_read16(b, kPad + 4 + 2 * i); }
s32 len(u8* b) { return static_cast<s32>(ps_read32(b, kPad)); }

} // namespace

TEST_CASE("cellPad: one virtual controller on port 0, data on change, press and sensor modes, host input", "[hle][io]")
{
    Fixture f;
    GuestThread g(f, 0, "pad");
    u8* b = f.base();

    CHECK(g.call("cellPadGetInfo2", kInfo) == kPadUninit);
    CHECK(g.call("cellPadInit", 0) == kPadInval);
    CHECK(g.call("cellPadInit", 128) == kPadInval);
    REQUIRE(g.call("cellPadInit", 7) == 0);
    CHECK(g.call("cellPadInit", 7) == kPadAlready);

    // Info: port 0 connected (with the assignment-changed flag the first time), a standard DualShock 3 device.
    REQUIRE(g.call("cellPadGetInfo2", kInfo) == 0);
    CHECK(ps_read32(b, kInfo + 0x0) == 7);    // max_connect
    CHECK(ps_read32(b, kInfo + 0x4) == 1);    // now_connect
    CHECK(ps_read32(b, kInfo + 0xC) == 3);    // port_status[0]: connected | assign changes
    CHECK(ps_read32(b, kInfo + 0x10) == 0);   // port_status[1]
    CHECK(ps_read32(b, kInfo + 0x44) == 0x1F); // device_capability[0]
    CHECK(ps_read32(b, kInfo + 0x60) == 0);   // device_type[0]: standard
    REQUIRE(g.call("cellPadGetInfo2", kInfo) == 0);
    CHECK(ps_read32(b, kInfo + 0xC) == 1); // the flag is reported once

    // The first read reports everything (at rest), then nothing until something changes.
    REQUIRE(g.call("cellPadGetData", 0, kPad) == 0);
    CHECK(len(b) == 8);
    CHECK(button(b, 0) == 0);
    CHECK(button(b, 1) == 0x74); // 0x7 << 4 | len / 2
    CHECK(button(b, 2) == 0);
    CHECK(button(b, 6) == 128); // left stick x at rest
    REQUIRE(g.call("cellPadGetData", 0, kPad) == 0);
    CHECK(len(b) == 0);
    CHECK(g.call("cellPadGetData", 1, kPad) == kPadNoDevice);
    CHECK(g.call("cellPadGetData", 0, 0) == kPadInval);

    // Host input: cross held, left stick pushed left.
    hle::PadInput in;
    in.digital2 = 0x40;
    in.left_x = 0;
    in.pressure[6] = 200; // cross
    hle::set_pad_input(*f.hle, in);
    REQUIRE(g.call("cellPadGetData", 0, kPad) == 0);
    CHECK(len(b) == 8);
    CHECK(button(b, 3) == 0x40);
    CHECK(button(b, 6) == 0);
    CHECK(button(b, 14) == 0); // pressure is only reported with press mode on

    // Press mode: 20 halfwords, pressures included.
    REQUIRE(g.call("cellPadSetPortSetting", 0, 2) == 0);
    in.pressure[6] = 255;
    hle::set_pad_input(*f.hle, in);
    REQUIRE(g.call("cellPadGetData", 0, kPad) == 0);
    CHECK(len(b) == 20);
    CHECK(button(b, 1) == 0x7A);
    CHECK(button(b, 14) == 255);

    // Sensor mode: 24 halfwords, at least every 10 ms even without a change.
    REQUIRE(g.call("cellPadSetPortSetting", 0, 6) == 0);
    REQUIRE(g.call("cellPadGetData", 0, kPad) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    REQUIRE(g.call("cellPadGetData", 0, kPad) == 0);
    CHECK(len(b) == 24);
    CHECK(button(b, 21) == 399); // y at rest: 1 G
    CHECK(g.call("cellPadSetPortSetting", 200, 0) == kPadInval);
    CHECK(g.call("cellPadSetPortSetting", 9, 0) == 0); // past the 7 ports: accepted, as RPCS3

    // Rumble: accepted; reserved bytes must be 0 (SDK above 2.0).
    ps_write8(b, kAct + 0, 1);
    ps_write8(b, kAct + 1, 128);
    CHECK(g.call("cellPadSetActDirect", 0, kAct) == 0);
    ps_write8(b, kAct + 5, 1);
    CHECK(g.call("cellPadSetActDirect", 0, kAct) == kPadInval);
    ps_write8(b, kAct + 5, 0);
    CHECK(g.call("cellPadSetActDirect", 1, kAct) == kPadNoDevice);

    REQUIRE(g.call("cellPadEnd") == 0);
    CHECK(g.call("cellPadEnd") == kPadUninit);
}

TEST_CASE("cellKb: initialized with no keyboard connected", "[hle][io]")
{
    Fixture f;
    GuestThread g(f, 0, "kb");
    u8* b = f.base();

    CHECK(g.call("cellKbGetInfo", kKb) == kKbUninit);
    CHECK(g.call("cellKbInit", 0) == kKbInval);
    REQUIRE(g.call("cellKbInit", 1) == 0); // GTA V's call
    ps_write32(b, kKb + 4, 0xDEADBEEF);
    REQUIRE(g.call("cellKbGetInfo", kKb) == 0);
    CHECK(ps_read32(b, kKb + 0) == 1); // max_connect
    CHECK(ps_read32(b, kKb + 4) == 0); // now_connect
    CHECK(ps_read8(b, kKb + 12) == 0); // status[0]: disconnected
    CHECK(g.call("cellKbRead", 0, kKb) == kKbNoDevice);
    CHECK(g.call("cellKbRead", 127, kKb) == kKbInval);
    CHECK(g.call("cellKbClearBuf", 0) == kKbNoDevice);
    CHECK(g.call("cellKbSetCodeType", 0, 1) == 0);
    CHECK(g.call("cellKbSetCodeType", 0, 2) == kKbInval);
    CHECK(g.call("cellKbSetReadMode", 0, 1) == 0);
    CHECK(g.call("cellKbSetReadMode", 0, 2) == kKbInval);
    CHECK(g.call("cellKbSetLEDStatus", 0, 8) == kKbSetting);
    CHECK(g.call("cellKbSetLEDStatus", 0, 1) == kKbFatal); // RPCS3: no keyboard on the port
    REQUIRE(g.call("cellKbEnd") == 0);
    CHECK(g.call("cellKbEnd") == kKbUninit);
}

TEST_CASE("cellPad: raw data, press and sensor modes, extra data, peripherals and the IIR filter", "[hle][io]")
{
    Fixture f;
    GuestThread g(f, 0, "pad2");
    u8* b = f.base();
    constexpr u64 kUnsupported = 0xFFFFFFFF80121108ull;

    CHECK(g.call("cellPadInfoPressMode", 0) == kPadUninit);
    REQUIRE(g.call("cellPadInit", 2) == 0);
    CHECK(g.call("cellPadGetRawData", 0, kPad) == 0);
    CHECK(g.call("cellPadGetRawData", 0, 0) == kPadInval);
    CHECK(g.call("cellPadGetRawData", 1, kPad) == kPadNoDevice);
    CHECK(g.call("cellPadGetRawData", 2, kPad) == kPadNoDevice); // past max_connect

    // The virtual controller has press-sensitive buttons and sensors: the answers are values, not codes.
    CHECK(g.call("cellPadInfoPressMode", 0) == 1);
    CHECK(g.call("cellPadInfoSensorMode", 0) == 1);
    CHECK(g.call("cellPadInfoPressMode", 1) == kPadNoDevice);
    CHECK(g.call("cellPadInfoSensorMode", 127) == kPadInval);
    REQUIRE(g.call("cellPadSetPressMode", 0, 1) == 0);
    REQUIRE(g.call("cellPadGetInfo2", kInfo) == 0);
    CHECK(ps_read32(b, kInfo + 0x28) == 2); // port_setting[0]: press on
    REQUIRE(g.call("cellPadSetSensorMode", 0, 1) == 0);
    REQUIRE(g.call("cellPadGetInfo2", kInfo) == 0);
    CHECK(ps_read32(b, kInfo + 0x28) == 6);
    REQUIRE(g.call("cellPadSetPressMode", 0, 0) == 0);
    REQUIRE(g.call("cellPadSetSensorMode", 0, 0) == 0);
    CHECK(g.call("cellPadSetPressMode", 0, 2) == kPadInval);
    CHECK(g.call("cellPadSetPressMode", 1, 1) == kUnsupported); // no controller there: no capability
    CHECK(g.call("cellPadSetSensorMode", 9, 1) == 0);           // past the 7 ports: accepted, as RPCS3

    // cellPadGetDataExtra: cellPadGetData, the device type, the BD remote's words cleared.
    std::memset(b + kPad, 0xAA, 0x84);
    ps_write32(b, kAct, 0xDEADBEEF);
    REQUIRE(g.call("cellPadGetDataExtra", 0, kAct, kPad) == 0);
    CHECK(len(b) == 8);
    CHECK(ps_read32(b, kAct) == 0); // CELL_PAD_DEV_TYPE_STANDARD
    CHECK(button(b, 24) == 0);
    CHECK(button(b, 25) == 0);
    CHECK(g.call("cellPadGetDataExtra", 0, 0, kPad) == 0); // no device type pointer: no error
    CHECK(len(b) == 0);
    CHECK(g.call("cellPadGetDataExtra", 1, kAct, kPad) == kPadNoDevice);

    // Peripherals: cellPadGetInfo2's fields then the classes; data with the class first.
    REQUIRE(g.call("cellPadClearBuf", 0) == 0);
    std::memset(b + kInfo, 0xAA, 0xB4);
    REQUIRE(g.call("cellPadPeriphGetInfo", kInfo) == 0);
    CHECK(ps_read32(b, kInfo + 0x0) == 2);  // max_connect
    CHECK(ps_read32(b, kInfo + 0x4) == 1);  // now_connect
    CHECK(ps_read32(b, kInfo + 0x44) == 0x1F);
    CHECK(ps_read32(b, kInfo + 0x7C) == 0); // pclass_type[0]: standard
    CHECK(ps_read32(b, kInfo + 0xB0) == 0); // pclass_profile[6]
    CHECK(g.call("cellPadPeriphGetInfo", 0) == kPadInval);
    ps_write32(b, kPad, 0xAAAAAAAA);
    REQUIRE(g.call("cellPadPeriphGetData", 0, kPad) == 0);
    CHECK(ps_read32(b, kPad + 0) == 0); // pclass_type
    CHECK(ps_read32(b, kPad + 8) == 8); // CellPadData.len after the buffer was cleared
    CHECK(g.call("cellPadPeriphGetData", 7, kPad) == kPadInval);
    CHECK(g.call("cellPadPeriphGetData", 1, kPad) == kPadNoDevice);

    CHECK(g.call("cellPadFilterIIRInit", 0, 0) == 0xFFFFFFFF80121401ull);
    CHECK(g.call("cellPadFilterIIRInit", kAct, 0) == 0);
    CHECK(g.call("cellPadFilterIIRFilter", kAct, 512) == 512);
    CHECK(g.call("cellPadFilterIIRFilter", kAct, 5000) == 1023);
    REQUIRE(g.call("cellPadEnd") == 0);
}

TEST_CASE("cellPad: a custom (LDD) controller the game registers and feeds", "[hle][io]")
{
    Fixture f;
    GuestThread g(f, 0, "ldd");
    u8* b = f.base();
    constexpr u64 kFatal = 0xFFFFFFFF80121101ull, kTooMany = 0xFFFFFFFF80121109ull;

    CHECK(g.call("cellPadLddRegisterController") == kPadUninit);
    REQUIRE(g.call("cellPadInit", 7) == 0);
    REQUIRE(g.call("cellPadGetInfo2", kInfo) == 0); // takes the virtual controller's assignment-changed flag
    const u64 handle = g.call("cellPadLddRegisterController");
    REQUIRE(handle == 1); // the first free port
    CHECK(g.call("cellPadLddGetPortNo", 1) == 1);
    CHECK(g.call("cellPadLddGetPortNo", 2) == kFatal);
    CHECK(g.call("cellPadLddGetPortNo", 7) == kPadInval);

    REQUIRE(g.call("cellPadGetInfo2", kInfo) == 0);
    CHECK(ps_read32(b, kInfo + 0x4) == 2);    // now_connect
    CHECK(ps_read32(b, kInfo + 0x10) == 7);   // port_status[1]: connected | assign changes | custom controller
    CHECK(ps_read32(b, kInfo + 0x48) == 1);   // device_capability[1]: PS3 conformity
    CHECK(ps_read32(b, kInfo + 0x64) == 5);   // device_type[1]: LDD

    // What the game inserts is what reads return, every time.
    std::memset(b + kAct, 0, 0x84);
    ps_write16(b, kAct + 4 + 2 * 2, 0x0010);
    ps_write16(b, kAct + 4 + 2 * 4, 0x00C0);
    REQUIRE(g.call("cellPadLddDataInsert", 1, kAct) == 0);
    for (int i = 0; i < 2; i++) {
        REQUIRE(g.call("cellPadGetData", 1, kPad) == 0);
        CHECK(len(b) == 8);
        CHECK(button(b, 2) == 0x0010);
        CHECK(button(b, 4) == 0x00C0);
    }
    CHECK(g.call("cellPadLddDataInsert", 1, 0) == kPadInval);
    CHECK(g.call("cellPadLddDataInsert", 2, kAct) == kPadNoDevice);

    for (u64 port = 2; port < 7; port++) CHECK(g.call("cellPadLddRegisterController") == port);
    CHECK(g.call("cellPadLddRegisterController") == kTooMany);

    REQUIRE(g.call("cellPadLddUnregisterController", 1) == 0);
    CHECK(g.call("cellPadLddUnregisterController", 1) == kPadNoDevice);
    CHECK(g.call("cellPadGetData", 1, kPad) == kPadNoDevice);
    CHECK(g.call("cellPadLddRegisterController") == 1); // the port is free again
    REQUIRE(g.call("cellPadEnd") == 0);
}

TEST_CASE("cellKb: configuration and raw code conversion", "[hle][io]")
{
    Fixture f;
    GuestThread g(f, 0, "kbcnv");

    CHECK(g.call("cellKbGetConfiguration", 0, kKb) == kKbUninit);
    REQUIRE(g.call("cellKbInit", 1) == 0);
    CHECK(g.call("cellKbGetConfiguration", 0, kKb) == kKbNoDevice);
    CHECK(g.call("cellKbGetConfiguration", 0, 0) == kKbNoDevice); // the device is checked before the pointer
    CHECK(g.call("cellKbGetConfiguration", 127, kKb) == kKbInval);
    REQUIRE(g.call("cellKbEnd") == 0);

    // cellKbCnvRawCode(arrange, mkey, led, rawcode) needs no keyboard (RPCS3's mapping).
    constexpr u64 k101 = 0, k106 = 1, kGerman = 3, kShift = 0x02, kAlt = 0x04, kNum = 0x01, kCaps = 0x02;
    CHECK(g.call("cellKbCnvRawCode", k101, 0, 0, 0x29) == 0x8029);       // escape: raw
    CHECK(g.call("cellKbCnvRawCode", k101, 0, 0, 0x39) == 0x8039);       // caps lock: raw
    CHECK(g.call("cellKbCnvRawCode", k101, 0, 0, 0x04) == 'a');
    CHECK(g.call("cellKbCnvRawCode", k101, kShift, 0, 0x04) == 'A');
    CHECK(g.call("cellKbCnvRawCode", k101, 0, kCaps, 0x04) == 'A');
    CHECK(g.call("cellKbCnvRawCode", k101, kShift, kCaps, 0x04) == 'a');
    CHECK(g.call("cellKbCnvRawCode", k101, kShift, 0, 0x1F) == '@');    // shift 2, US
    CHECK(g.call("cellKbCnvRawCode", k106, kShift, 0, 0x1F) == '"');    // shift 2, Japanese
    CHECK(g.call("cellKbCnvRawCode", k101, 0, kCaps, 0x1F) == '2');     // caps lock leaves digits alone
    CHECK(g.call("cellKbCnvRawCode", kGerman, 0, kCaps, 0x1F) == '"');  // ... except as a shift lock
    CHECK(g.call("cellKbCnvRawCode", kGerman, kAlt, 0, 0x14) == '@');   // AltGr Q
    CHECK(g.call("cellKbCnvRawCode", kGerman, 0, 0, 0x92) == 225);      // ß
    CHECK(g.call("cellKbCnvRawCode", k101, 0, kNum, 0x59) == 0x4031);   // keypad 1 with num lock
    CHECK(g.call("cellKbCnvRawCode", k101, 0, kNum, 0x58) == 0x400A);   // keypad enter
    CHECK(g.call("cellKbCnvRawCode", k101, 0, 0, 0x27) == '0');
    CHECK(g.call("cellKbCnvRawCode", k101, 0, 0, 0x28) == '\n');
    CHECK(g.call("cellKbCnvRawCode", k101, 0, 0, 0x2C) == ' ');
    CHECK(g.call("cellKbCnvRawCode", k101, 0, 0, 0x3A) == 0x803A); // F1: raw (0x39 to 0x53 are)
    CHECK(g.call("cellKbCnvRawCode", k101, 0, 0, 0x64) == 0);      // a key the layout lacks: no character
}

TEST_CASE("cellMouse: initialized with no mouse connected; sys_config", "[hle][io]")
{
    Fixture f;
    GuestThread g(f, 0, "mouse");
    u8* b = f.base();
    constexpr u64 kFatal = 0xFFFFFFFF80121201ull, kInval = 0xFFFFFFFF80121202ull,
                  kAlready = 0xFFFFFFFF80121203ull, kUninit = 0xFFFFFFFF80121204ull,
                  kNoDevice = 0xFFFFFFFF80121207ull;

    CHECK(g.call("cellMouseGetInfo", kKb) == kUninit);
    CHECK(g.call("cellMouseEnd") == kUninit);
    CHECK(g.call("cellMouseInit", 0) == kInval);
    CHECK(g.call("cellMouseInit", 128) == kInval);
    REQUIRE(g.call("cellMouseInit", 2) == 0);
    CHECK(g.call("cellMouseInit", 2) == kAlready);

    std::memset(b + kKb, 0xAA, 648);
    REQUIRE(g.call("cellMouseGetInfo", kKb) == 0);
    CHECK(ps_read32(b, kKb + 0) == 2); // max_connect
    CHECK(ps_read32(b, kKb + 4) == 0); // now_connect
    CHECK(ps_read8(b, kKb + 12 + 2 * 127 * 2) == 0); // status[0]
    CHECK(ps_read8(b, kKb + 647) == 0);
    CHECK(g.call("cellMouseGetInfo", 0) == kInval);

    for (const char* fn : {"cellMouseGetData", "cellMouseGetDataList", "cellMouseGetTabletDataList",
                           "cellMouseGetRawData"}) {
        INFO(fn);
        CHECK(g.call(fn, 0, kPad) == kNoDevice);
        CHECK(g.call(fn, 0, 0) == kInval);
        CHECK(g.call(fn, 127, kPad) == kInval);
    }
    CHECK(g.call("cellMouseClearBuf", 0) == kNoDevice);
    CHECK(g.call("cellMouseClearBuf", 127) == kInval);
    CHECK(g.call("cellMouseInfoTabletMode", 0, 0) == kFatal); // RPCS3: the port only, then FATAL
    CHECK(g.call("cellMouseSetTabletMode", 0, 9) == kFatal);
    CHECK(g.call("cellMouseSetTabletMode", 127, 1) == kInval);
    REQUIRE(g.call("cellMouseEnd") == 0);

    CHECK(g.call("sys_config_start") == 0);
    CHECK(g.call("sys_config_stop") == 0);
    CHECK(g.call("sys_config_stop") == 0); // RPCS3: an unbalanced stop is accepted
}
