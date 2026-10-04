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
