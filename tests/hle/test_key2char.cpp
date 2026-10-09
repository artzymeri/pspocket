// cellKey2char (D-565): keyboard key codes to characters, pspocket's own (made from RPCS3's checks and pspocket's
// cellKbCnvRawCode; no Sony module to compare with). Synthetic key data only. No game data.

#include "hle_test_util.hpp"

#include <cstring>
#include <string_view>

#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kHandle = kData + 0x100, kKey = kData + 0x200, kCharCode = kData + 0x210, kCharNum = kData + 0x214,
              kProcessed = kData + 0x218;
const u64 kInvalidHandle = r3_of_code(0x80121302), kInvalidParameter = r3_of_code(0x80121303),
          kAlreadyInitialized = r3_of_code(0x80121304), kUninitialized = r3_of_code(0x80121305),
          kOther = r3_of_code(0x80121306);
constexpr u32 kShift = 0x02, kRightShift = 0x20, kNumLock = 0x01, kCapsLock = 0x02;

struct Typed {
    u64 result;
    u32 count;
    bool processed;
    u16 ch;
};

Typed type(GuestThread& g, u16 keycode, u32 mkey = 0, u32 led = 0)
{
    u8* b = g.f.base();
    ps_write32(b, kKey, led);
    ps_write32(b, kKey + 4, mkey);
    ps_write16(b, kKey + 8, keycode);
    ps_write32(b, kCharCode, 0);
    ps_write32(b, kCharNum, 0xFFFFFFFF);
    ps_write8(b, kProcessed, 0xFF);
    const u64 r = g.call("cellKey2CharGetChar", kHandle, kKey, kCharCode, kCharNum, kProcessed);
    const u32 at = ps_read32(b, kCharCode);
    return {r, ps_read32(b, kCharNum), ps_read8(b, kProcessed) != 0, at ? ps_read16(b, at) : u16{0}};
}

} // namespace

TEST_CASE("cellKey2char: the five functions, firmware first", "[hle][key2char]")
{
    for (const std::string_view name : {"cellKey2CharOpen", "cellKey2CharClose", "cellKey2CharGetChar",
             "cellKey2CharSetMode", "cellKey2CharSetArrangement"}) {
        INFO(name);
        const hle::Function* fn = hle::registry().find("cellKey2char", recomp::nid_of(name));
        REQUIRE(fn != nullptr);
        CHECK(fn->name == name);
    }
    CHECK(hle::registry().is_firmware_first("cellKey2char"));
}

TEST_CASE("cellKey2char: open and close; the mode and the arrangement and their checks", "[hle][key2char]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    std::memset(b + kHandle, 0, 128);
    CHECK(g.call("cellKey2CharOpen", 0) == kInvalidHandle);
    CHECK(g.call("cellKey2CharClose", kHandle) == kUninitialized);
    CHECK(g.call("cellKey2CharSetMode", kHandle, 0) == kUninitialized);
    CHECK(g.call("cellKey2CharSetArrangement", kHandle, 0) == kUninitialized);
    CHECK(type(g, 0x04).result == kUninitialized);
    REQUIRE(g.call("cellKey2CharOpen", kHandle) == 0);
    CHECK(ps_read8(b, kHandle + 8) == 1); // opened
    CHECK(ps_read8(b, kHandle + 1) == 0); // the system's keyboard: 101 (US)
    CHECK(g.call("cellKey2CharOpen", kHandle) == kAlreadyInitialized);

    CHECK(g.call("cellKey2CharSetMode", 0, 0) == kInvalidHandle);
    CHECK(g.call("cellKey2CharSetMode", kHandle, 3) == kInvalidParameter);
    CHECK(g.call("cellKey2CharSetMode", kHandle, static_cast<u32>(-1)) == kInvalidParameter);
    REQUIRE(g.call("cellKey2CharSetMode", kHandle, 1) == 0);
    CHECK(ps_read8(b, kHandle + 2) == 1);
    CHECK(type(g, 0x04).ch == 'a'); // native modes type as the English one
    REQUIRE(g.call("cellKey2CharSetMode", kHandle, 0) == 0);
    CHECK(g.call("cellKey2CharSetArrangement", kHandle, 25) == kInvalidParameter);
    CHECK(g.call("cellKey2CharSetArrangement", kHandle, static_cast<u32>(-1)) == kInvalidParameter);
    REQUIRE(g.call("cellKey2CharSetArrangement", kHandle, 24) == 0);
    CHECK(ps_read8(b, kHandle + 1) == 24);

    // A handle marked broken (byte 0 = 255) answers OTHER.
    ps_write8(b, kHandle, 255);
    CHECK(g.call("cellKey2CharSetMode", kHandle, 0) == kOther);
    CHECK(type(g, 0x04).result == kOther);
    ps_write8(b, kHandle, 0);

    REQUIRE(g.call("cellKey2CharClose", kHandle) == 0);
    CHECK(ps_read8(b, kHandle + 8) == 0);
    CHECK(g.call("cellKey2CharClose", kHandle) == kUninitialized);
    REQUIRE(g.call("cellKey2CharOpen", kHandle) == 0); // and again
    CHECK(g.call("cellKey2CharClose", kHandle) == 0);
}

TEST_CASE("cellKey2char: keys to characters in the 101, 106 and German layouts", "[hle][key2char]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    std::memset(b + kHandle, 0, 128);
    REQUIRE(g.call("cellKey2CharOpen", kHandle) == 0);

    // The checks: no key data, no character pointer, key code 0.
    CHECK(g.call("cellKey2CharGetChar", kHandle, 0, kCharCode, kCharNum, kProcessed) == kInvalidParameter);
    CHECK(g.call("cellKey2CharGetChar", kHandle, kKey, 0, kCharNum, kProcessed) == kInvalidParameter);
    CHECK(type(g, 0).result == kInvalidParameter);

    // A character: one, processed, kept in the handle (from byte 0x40).
    Typed t = type(g, 0x04);
    CHECK(t.result == 0);
    CHECK(t.count == 1);
    CHECK(t.processed);
    CHECK(t.ch == 'a');
    CHECK(ps_read32(b, kCharCode) == kHandle + 0x40);
    CHECK(type(g, 0x04, kShift).ch == 'A');
    CHECK(type(g, 0x1D, kRightShift).ch == 'Z');
    CHECK(type(g, 0x04, 0, kCapsLock).ch == 'A');
    CHECK(type(g, 0x04, kShift, kCapsLock).ch == 'a');
    CHECK(type(g, 0x1E).ch == '1');
    CHECK(type(g, 0x1F, kShift).ch == '@'); // 101
    CHECK(type(g, 0x27).ch == '0');
    CHECK(type(g, 0x28).ch == 0x0A); // Enter
    CHECK(type(g, 0x2A).ch == 0x08); // Backspace
    CHECK(type(g, 0x2C).ch == ' ');
    CHECK(type(g, 0x2D, kShift).ch == '_');
    CHECK(type(g, 0x59, 0, kNumLock).ch == '1'); // keypad 1 with num lock (its keypad flag dropped)
    CHECK(type(g, 0x54, 0, kNumLock).ch == '/');
    // A key that types nothing: no character, not processed.
    t = type(g, 0x29); // Escape
    CHECK(t.result == 0);
    CHECK(t.count == 0);
    CHECK_FALSE(t.processed);
    t = type(g, 0x59); // keypad 1 without num lock
    CHECK(t.count == 0);
    CHECK_FALSE(t.processed);
    // charNum and processed may be left out.
    CHECK(g.call("cellKey2CharGetChar", kHandle, kKey, kCharCode, 0, 0) == 0);

    // 106 (Japanese): shift and 2 is '"', the yen key is U+00A5.
    REQUIRE(g.call("cellKey2CharSetArrangement", kHandle, 1) == 0);
    CHECK(type(g, 0x1F, kShift).ch == '"');
    CHECK(type(g, 0x89).ch == 0x00A5);
    CHECK(type(g, 0x89, kShift).ch == '|');
    // German: sharp s (U+00DF), shift 3 the section sign (U+00A7), caps lock a shift lock.
    REQUIRE(g.call("cellKey2CharSetArrangement", kHandle, 3) == 0);
    CHECK(type(g, 0x92).ch == 0x00DF);
    CHECK(type(g, 0x20, kShift).ch == 0x00A7);
    CHECK(type(g, 0x1F, 0, kCapsLock).ch == '"');
    CHECK(g.call("cellKey2CharClose", kHandle) == 0);
}
