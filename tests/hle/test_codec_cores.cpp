// The video codec cores' version queries (D-575): RPCS3's answers, the ARG errors, firmware first (but the cores
// pspocket has whole, D-920). No game data.

#include "hle_test_util.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;

TEST_CASE("Codec cores: versions and characteristics as RPCS3 has them, firmware first", "[hle][codec-cores]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    constexpr u32 kOut = kData + 0x100, kNotGuest = 0xF0000000u;

    struct Version {
        const char* name;
        u32 value, arg_error;
    };
    for (const Version& v : {Version{"sjvtdGetVersionNumber", 0x1050000, 0x80615841},
             Version{"smvd2GetVersionNumber", 0x1030000, 0x80615041},
             Version{"smvd4GetVersionNumber", 0x1080000, 0x80615141},
             Version{"svc1dGetVersionNumber", 0x1090000, 0x80615341}}) {
        INFO(v.name);
        ps_write32(b, kOut, 0);
        CHECK(g.call(v.name, kOut) == 0);
        CHECK(ps_read32(b, kOut) == v.value);
        CHECK(g.call(v.name, 0) == r3_of_code(v.arg_error));
        CHECK(g.call(v.name, kNotGuest) == r3_of_code(v.arg_error));
    }

    // The AVC and MVC cores' GetVersion answer nothing; without a pointer they write nothing.
    g.call("avcDecGetVersion", kOut);
    CHECK(ps_read32(b, kOut) == 0x11300);
    g.call("mvcDecGetVersion", kOut);
    CHECK(ps_read32(b, kOut) == 0x10804);
    g.call("avcDecGetVersion", 0);
    g.call("mvcDecGetVersion", kNotGuest);

    CHECK(g.call("avcDecQueryCharacteristics", kOut) == 0);
    CHECK(ps_read32(b, kOut) == 5);
    CHECK(ps_read32(b, kOut + 4) == 20);
    CHECK(g.call("mvcDecQueryCharacteristics", kOut) == 0);
    CHECK(ps_read32(b, kOut) == 5);
    CHECK(ps_read32(b, kOut + 4) == 36);
    CHECK(g.call("avcDecQueryCharacteristics", 0) == r3_of_code(0x80615201));
    CHECK(g.call("mvcDecQueryCharacteristics", kNotGuest) == r3_of_code(0x80615901));

    // The cores whose decoding calls are not pspocket's own yet; libsmvd4's, libsmvd2's and libsvc1d's are (D-920),
    // libavcdec's (D-933).
    for (const char* library : {"libmvcdec", "libsjvtd"}) {
        INFO(library);
        CHECK(hle::registry().is_firmware_first(library));
    }
    CHECK_FALSE(hle::registry().is_firmware_first("libsmvd4"));
    CHECK_FALSE(hle::registry().is_firmware_first("libsmvd2"));
    CHECK_FALSE(hle::registry().is_firmware_first("libsvc1d"));
    CHECK_FALSE(hle::registry().is_firmware_first("libavcdec"));
}
