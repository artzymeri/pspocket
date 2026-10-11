// cellKey2char (D-565, D-914): keyboard key codes to characters, as Sony's libkey2char makes them (compared in
// test_key2char_compare.cpp; these are its answers, checked without the firmware). The characters come through
// pspocket's cellKbCnvRawCode (sys_io), which both halves of the comparison share. Synthetic key data only. No game
// data.

#include "hle_test_util.hpp"

#include <cstring>
#include <format>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kHandle = kData + 0x100, kKey = kData + 0x200, kCharCode = kData + 0x210, kCharNum = kData + 0x214,
              kProcessed = kData + 0x218, kAuto = kData + 0x300;
const u64 kInvalidHandle = r3_of_code(0x80121302), kInvalidParameter = r3_of_code(0x80121303),
          kAlreadyInitialized = r3_of_code(0x80121304), kUninitialized = r3_of_code(0x80121305),
          kOther = r3_of_code(0x80121306);
constexpr u32 kShift = 0x02, kRightShift = 0x20, kNumLock = 0x01, kCapsLock = 0x02;

struct Typed {
    u64 result;
    u32 count;
    bool processed;
    std::vector<u16> chars; // count of them
};

Typed type(GuestThread& g, u16 keycode, u32 mkey = 0, u32 led = 0)
{
    u8* b = g.f.base();
    ps_write32(b, kKey, led);
    ps_write32(b, kKey + 4, mkey);
    ps_write16(b, kKey + 8, keycode);
    ps_write32(b, kCharCode, 0);
    ps_write32(b, kCharNum, 0xFFFFFFFF);
    ps_write8(b, kProcessed, 0xEE);
    const u64 r = g.call("cellKey2CharGetChar", kHandle, kKey, kCharCode, kCharNum, kProcessed);
    Typed t{r, ps_read32(b, kCharNum), ps_read8(b, kProcessed) == 1, {}};
    const u32 at = ps_read32(b, kCharCode);
    for (u32 i = 0; at && i < t.count && i < 5; i++) t.chars.push_back(ps_read16(b, at + 2 * i));
    return t;
}

std::vector<u16> chars(std::initializer_list<u16> c) { return c; }

struct Env {
    Fixture f;
    GuestThread g;
    u8* b;
    Env() : g(f, 0, "game"), b(f.base())
    {
        std::memset(b + kHandle, 0, 128);
        REQUIRE(g.call("cellKey2CharOpen", kHandle) == 0);
    }
    void arrangement(u32 arr, u32 mode = 0)
    {
        REQUIRE(g.call("cellKey2CharSetArrangement", kHandle, arr) == 0);
        REQUIRE(g.call("cellKey2CharSetMode", kHandle, mode) == 0);
    }
    // A dead key waiting in the handle (the game's memory), as typing one leaves it.
    void dead(u8 c)
    {
        ps_write8(b, kHandle + 2, 1);
        ps_write8(b, kHandle + 4, c);
    }
};

} // namespace

TEST_CASE("cellKey2char: the five functions and the nine known by NID, firmware first", "[hle][key2char]")
{
    for (const std::string_view name : {"cellKey2CharOpen", "cellKey2CharClose", "cellKey2CharGetChar",
             "cellKey2CharSetMode", "cellKey2CharSetArrangement"}) {
        INFO(name);
        const hle::Function* fn = hle::registry().find("cellKey2char", recomp::nid_of(name));
        REQUIRE(fn != nullptr);
        CHECK(fn->name == name);
    }
    for (const u32 nid : {0x83E91458u, 0xAE4B34E6u, 0xE5D1F9B0u, 0x291ADBA5u, 0x1B5F6EFCu, 0x51667234u, 0x7F8E6F3Eu,
             0x28893D81u, 0xEAE114F6u})
        CHECK(hle::registry().find("cellKey2char", nid) != nullptr);
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
    // No keyboard is connected: Open finds no arrangement and marks the handle (byte 0 = 255).
    REQUIRE(g.call("cellKey2CharOpen", kHandle) == 0);
    CHECK(ps_read8(b, kHandle + 8) == 1);
    CHECK(ps_read8(b, kHandle) == 0xFF);
    CHECK(g.call("cellKey2CharOpen", kHandle) == kAlreadyInitialized);
    CHECK(g.call("cellKey2CharSetMode", kHandle, 3) == kInvalidParameter);
    CHECK(g.call("cellKey2CharSetMode", kHandle, 0) == kOther);
    CHECK(type(g, 0x04).result == kOther);
    // SetArrangement does not look at the mark: it sets byte 0 again.
    CHECK(g.call("cellKey2CharSetArrangement", kHandle, 25) == kInvalidParameter);
    CHECK(g.call("cellKey2CharSetArrangement", kHandle, static_cast<u32>(-1)) == kInvalidParameter);
    REQUIRE(g.call("cellKey2CharSetArrangement", kHandle, 24) == 0);
    CHECK(ps_read8(b, kHandle) == 24);
    CHECK(ps_read8(b, kHandle + 1) == 24);
    // An arrangement without a native mode keeps no mode; mode 2 is traditional Chinese's only.
    REQUIRE(g.call("cellKey2CharSetMode", kHandle, 1) == 0);
    CHECK(ps_read8(b, kHandle + 3) == 0);
    CHECK(g.call("cellKey2CharSetMode", kHandle, 2) == kInvalidParameter);
    CHECK(g.call("cellKey2CharSetMode", 0, 0) == kInvalidHandle);
    // 106 kana counts as 106; Russian converts in the US layout until its native mode.
    REQUIRE(g.call("cellKey2CharSetArrangement", kHandle, 2) == 0);
    CHECK(ps_read8(b, kHandle) == 1);
    CHECK(ps_read8(b, kHandle + 1) == 1);
    REQUIRE(g.call("cellKey2CharSetMode", kHandle, 1) == 0);
    CHECK(ps_read8(b, kHandle + 3) == 1);
    REQUIRE(g.call("cellKey2CharSetArrangement", kHandle, 9) == 0);
    CHECK(ps_read8(b, kHandle) == 0);
    CHECK(ps_read8(b, kHandle + 3) == 0);
    REQUIRE(g.call("cellKey2CharSetMode", kHandle, 1) == 0);
    CHECK(ps_read8(b, kHandle) == 9);
    REQUIRE(g.call("cellKey2CharSetMode", kHandle, 0) == 0);
    CHECK(ps_read8(b, kHandle) == 0);
    // Traditional Chinese set twice: its conversion arrangement goes back to 0.
    REQUIRE(g.call("cellKey2CharSetArrangement", kHandle, 16) == 0);
    CHECK(ps_read8(b, kHandle) == 16);
    REQUIRE(g.call("cellKey2CharSetArrangement", kHandle, 16) == 0);
    CHECK(ps_read8(b, kHandle) == 0);
    REQUIRE(g.call("cellKey2CharSetMode", kHandle, 2) == 0);
    CHECK(ps_read8(b, kHandle + 3) == 2);
    // Korean: the automaton set up in the handle, its stacks' data in it too.
    REQUIRE(g.call("cellKey2CharSetArrangement", kHandle, 11) == 0);
    CHECK(ps_read8(b, kHandle) == 0);
    CHECK(ps_read32(b, kHandle + 0x14) == 1);
    CHECK(ps_read32(b, kHandle + 0x18) == 1); // the state: nothing composed
    CHECK(ps_read32(b, kHandle + 0x2C) == kHandle + 0x3C);
    CHECK(ps_read16(b, kHandle + 0x32) == 6);
    CHECK(ps_read32(b, kHandle + 0x34) == kHandle + 0x54);

    REQUIRE(g.call("cellKey2CharClose", kHandle) == 0);
    CHECK(ps_read8(b, kHandle + 8) == 0);
    CHECK(g.call("cellKey2CharClose", kHandle) == kUninitialized);
}

TEST_CASE("cellKey2char: keys to characters; dead keys; the code pages", "[hle][key2char]")
{
    Env e;
    GuestThread& g = e.g;
    e.arrangement(0);
    CHECK(g.call("cellKey2CharGetChar", kHandle, 0, kCharCode, kCharNum, kProcessed) == kInvalidParameter);
    CHECK(g.call("cellKey2CharGetChar", kHandle, kKey, 0, kCharNum, kProcessed) == kInvalidParameter);
    CHECK(type(g, 0).result == kInvalidParameter);

    Typed t = type(g, 0x04);
    CHECK(t.result == 0);
    CHECK(t.processed);
    CHECK(t.chars == chars({'a'}));
    CHECK(ps_read32(e.b, kCharCode) == kHandle + 10); // the handle's characters
    CHECK(type(g, 0x04, kShift).chars == chars({'A'}));
    CHECK(type(g, 0x1D, kRightShift).chars == chars({'Z'}));
    CHECK(type(g, 0x04, 0, kCapsLock).chars == chars({'A'}));
    CHECK(type(g, 0x1F, kShift).chars == chars({'@'}));
    CHECK(type(g, 0x28).chars == chars({0x0A}));
    CHECK(type(g, 0x2A).chars == chars({0x08}));
    CHECK(type(g, 0x59, 0, kNumLock).chars == chars({'1'})); // the keypad flag dropped
    // A key that types nothing (cellKbCnvRawCode's raw flag): its key code, not processed, no character counted.
    t = type(g, 0x29);
    CHECK(t.result == 0);
    CHECK(t.count == 0);
    CHECK_FALSE(t.processed);
    CHECK(ps_read16(e.b, kHandle + 10) == 0x29);
    // A key cellKbCnvRawCode gives nothing for: OTHER.
    CHECK(type(g, 0x59).result == kOther); // the keypad without num lock
    CHECK(type(g, 0x104).result == kOther);

    // German: ^ and ` are dead keys; a letter composes, another goes with them, Backspace too.
    e.arrangement(3);
    t = type(g, 0x2E); // ^
    CHECK(t.result == 0);
    CHECK(t.count == 0);
    CHECK(t.processed);
    CHECK(type(g, 0x04).chars == chars({0xE2}));                  // â
    CHECK(type(g, 0x93, kShift).count == 0);                      // ` (sys_io's code for the key left of Backspace)
    CHECK(type(g, 0x08, kShift).chars == chars({0xC8}));          // È
    CHECK(type(g, 0x2E).count == 0);
    CHECK(type(g, 0x1B).chars == chars({'^', 'x'}));              // x does not compose
    CHECK(type(g, 0x2E).count == 0);
    CHECK(type(g, 0x2A).chars == chars({'^', 0x08}));             // nor Backspace
    // Each dead key's letters, and the code pages: Polish's ~ is the ogonek key (Windows-1250); Turkish reads
    // Windows-1254; Russian's native mode the upper half of ISO 8859-5.
    e.dead(0xB4);
    CHECK(type(g, 0x1C).chars == chars({0xFD}));                  // ý
    e.dead(0xA8);
    CHECK(type(g, 0x1C, kShift).chars == chars({0x178}));         // Ÿ (Windows-1252's 0x9F)
    e.dead(0xB8);
    CHECK(type(g, 0x06).chars == chars({0xE7}));                  // ç
    e.dead('~');
    CHECK(type(g, 0x11).chars == chars({0xF1}));                  // ñ
    e.arrangement(22);
    e.dead('~');
    CHECK(type(g, 0x04).chars == chars({0x105}));                 // ą
    e.dead('~');
    CHECK(type(g, 0x1B, kShift).chars == chars({0x179}));         // Ź
    e.arrangement(24);
    e.dead(0xB4);
    CHECK(type(g, 0x1C).chars == chars({0x131}));                 // ı (Windows-1254's 0xFD)
    e.arrangement(9, 1);
    e.dead(0xB4);
    CHECK(type(g, 0x04).chars == chars({0x441}));                 // 0xE1 in ISO 8859-5
    e.dead(0xF0);
    CHECK(type(g, 0x2A).chars == chars({0x2116, 0x08}));          // an unknown dead key and Backspace: numero sign
    e.dead(0xF0);
    CHECK(type(g, 0x04).count == 0);                              // an unknown dead key: nothing
    // 106: cellKbCnvRawCode's yen sign (code page 850's 190) read as Windows-1252.
    e.arrangement(1);
    CHECK(type(g, 0x89).chars == chars({0xBE}));
}

TEST_CASE("cellKey2char: Japanese kana with dakuten and handakuten", "[hle][key2char]")
{
    Env e;
    GuestThread& g = e.g;
    e.arrangement(1, 1);
    CHECK(type(g, 0x17).chars == chars({0x304B}));         // ka
    CHECK(type(g, 0x2F).chars == chars({0x08, 0x304C}));   // ga, the ka shown replaced
    CHECK(type(g, 0x2F).chars == chars({0x309B}));         // the mark alone
    CHECK(type(g, 0x09).chars == chars({0x306F}));         // ha
    CHECK(type(g, 0x30).chars == chars({0x08, 0x3071}));   // pa
    CHECK(type(g, 0x20).chars == chars({0x3042}));         // a takes no mark
    CHECK(type(g, 0x30).chars == chars({0x309C}));
    CHECK(type(g, 0x30, kShift).chars == chars({0x300C})); // shifted: a bracket, no mark
    CHECK(type(g, 0x27, kShift).chars == chars({0x3092})); // wo
    CHECK(type(g, 0x20, kShift).chars == chars({0x3041})); // small a
    CHECK(type(g, 0x2C).chars == chars({' '}));
    CHECK(type(g, 0x2D).result == kOther);                 // cellKbCnvRawCode has nothing for it in 106
}

TEST_CASE("cellKey2char: Korean, composed through the automaton", "[hle][key2char]")
{
    Env e;
    GuestThread& g = e.g;
    e.arrangement(11, 1);
    // 안녕하세요: each key the syllable as it grows (a backspace first replacing the one shown), a syllable completed
    // with the next one's start.
    const std::pair<u16, std::vector<u16>> words[] = {{0x07, {0x3147}}, {0x0E, {8, 0xC544}}, {0x16, {8, 0xC548}},
        {0x16, {8, 0xC548, 0x3134}}, {0x18, {8, 0xB140}}, {0x07, {8, 0xB155}}, {0x0A, {8, 0xB155, 0x314E}},
        {0x0E, {8, 0xD558}}, {0x17, {8, 0xD56B}}, {0x13, {8, 0xD558, 0xC138}}, {0x07, {8, 0xC14D}},
        {0x1C, {8, 0xC138, 0xC694}}, {0x2C, {' '}}};
    for (const auto& [key, want] : words) {
        INFO(std::format("key {:#x}", key));
        const Typed t = type(g, key);
        CHECK(t.result == 0);
        CHECK(t.processed);
        CHECK(t.chars == want);
    }
    // 한글, then backspaces through 글 and on.
    for (const u16 key : {0x0A, 0x0E, 0x16, 0x15, 0x10, 0x09}) type(g, key);
    CHECK(type(g, 0x2A).chars == chars({8, 0xADF8})); // 그
    CHECK(type(g, 0x2A).chars == chars({8, 0x3131})); // ㄱ
    CHECK(type(g, 0x2A).chars == chars({8}));         // nothing left: the backspace goes to the game
    CHECK(type(g, 0x2A).chars == chars({8}));
    // Doubled consonants shifted; keys that are no jamo; Escape and F1 not processed.
    CHECK(type(g, 0x15, kShift).chars == chars({0x3132}));
    CHECK(type(g, 0x1E).chars == chars({'1'}));
    Typed t = type(g, 0x29);
    CHECK_FALSE(t.processed);
    CHECK(t.count == 0);
    CHECK(type(g, 0x0E).chars == chars({0x314F}));
    CHECK(type(g, 0x59, 0, kNumLock).chars == chars({'1'}));
    CHECK(type(g, 0x104).chars == chars({0})); // nothing the layout has: a 0, processed
}

TEST_CASE("cellKey2char: traditional Chinese, Zhuyin and Cangjie", "[hle][key2char]")
{
    Env e;
    GuestThread& g = e.g;
    e.arrangement(16, 1);
    CHECK(type(g, 0x1E).chars == chars({0x3105}));        // ㄅ
    CHECK(type(g, 0x23).chars == chars({0x02CA}));        // the second tone
    CHECK(type(g, 0x04, kShift).chars == chars({'a'}));   // shifted: the letter
    e.arrangement(16, 2);
    CHECK(type(g, 0x04).chars == chars({0x65E5}));        // 日
    Typed t = type(g, 0x1D);                              // Z: no radical
    CHECK_FALSE(t.processed);
    CHECK(t.count == 0);
    CHECK(type(g, 0x04, kShift).chars == chars({'a'}));   // the case inverted
    CHECK(type(g, 0x04, kShift, kCapsLock).chars == chars({'A'}));
    CHECK(type(g, 0x2C).count == 0);                      // Cangjie's table has no space
}

TEST_CASE("cellKey2char: the Hangul automaton's functions", "[hle][key2char]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    const auto call = [&](const char* name, std::initializer_list<u64> args) {
        const hle::Function* fn = nullptr;
        for (const hle::Function& x : hle::registry().functions())
            if (x.module == "cellKey2char" && x.name == name) fn = &x;
        REQUIRE(fn != nullptr);
        u32 i = 3;
        for (const u64 a : args) g.ctx.gpr[i++] = a;
        fn->fn(g.ctx, b);
        return static_cast<u32>(g.ctx.gpr[3]);
    };
    CHECK(call("cellKey2char_28893D81", {0x1112, 0x1161, 0x11AB}) == 0xD55C); // 한
    CHECK(call("cellKey2char_28893D81", {0x1112, 0x1161, 0x11A8}) == 0xD559);
    CHECK(call("cellKey2char_28893D81", {0x115F, 0x1161, 0x11A7}) == 0x1161); // a medial alone
    CHECK(call("cellKey2char_28893D81", {0x1100, 0x1160, 0x11A7}) == 0x1100);
    CHECK(call("cellKey2char_EAE114F6", {0x1100}) == 0x3131);
    CHECK(call("cellKey2char_EAE114F6", {0x1161}) == 0x314F);
    CHECK(call("cellKey2char_EAE114F6", {0x11C2}) == 0x314E);
    CHECK(call("cellKey2char_EAE114F6", {0x1176}) == 0);
    // An automaton of the test's: init answers its second stack's address (r3 as libkey2char leaves it).
    std::memset(b + kAuto, 0xA5, 0x60);
    CHECK(call("cellKey2char_83E91458", {kAuto}) == kAuto + 0x20);
    CHECK(ps_read32(b + 0, kAuto + 0x18) == kAuto + 0x28);
    CHECK(ps_read32(b, kAuto + 4) == 1);
    // ᄒ ᅡ ᄂ: state 2, 3, then the final (ᆫ) after a medial: 5.
    CHECK(call("cellKey2char_51667234", {kAuto, 0x1112}) == 0);
    CHECK(ps_read32(b, kAuto + 4) == 2);
    CHECK(call("cellKey2char_51667234", {kAuto, 0x1161}) == 0);
    CHECK(ps_read32(b, kAuto + 4) == 3);
    CHECK(call("cellKey2char_51667234", {kAuto, 0x1102}) == 0);
    CHECK(ps_read32(b, kAuto + 4) == 5);
    CHECK(ps_read32(b, kAuto + 0x10) == 0x11AB);
    // ᅵ: the syllable ends without its final, which starts the next (하 | 니).
    CHECK(call("cellKey2char_51667234", {kAuto, 0x1175}) == 1);
    CHECK(ps_read32(b, kAuto + 8) == 0x1112);
    CHECK(ps_read32(b, kAuto + 0x0C) == 0x1161);
    CHECK(ps_read32(b, kAuto + 0x10) == 0x11A7);
    CHECK(ps_read32(b, kAuto + 0x28) == 0x1102);
    CHECK(ps_read32(b, kAuto + 0x2C) == 0x1175);
    // Backspace, the stack put back, recompute, reset, close.
    CHECK(call("cellKey2char_1B5F6EFC", {kAuto}) == 1);
    CHECK(ps_read16(b, kAuto + 0x1C) == 1);
    CHECK(call("cellKey2char_291ADBA5", {kAuto}) == 1);
    CHECK(call("cellKey2char_7F8E6F3E", {kAuto}) == kAuto + 0x18);
    CHECK(ps_read32(b, kAuto + 4) == 1);
    CHECK(call("cellKey2char_1B5F6EFC", {kAuto}) == 0); // nothing to take off
    CHECK(call("cellKey2char_E5D1F9B0", {kAuto}) == 0); // an empty stack: its count
    CHECK(call("cellKey2char_AE4B34E6", {kAuto}) == kAuto + 0x18);
    CHECK(ps_read16(b, kAuto + 0x1E) == 0);
    CHECK(call("cellKey2char_AE4B34E6", {0}) == 0x20);
}
