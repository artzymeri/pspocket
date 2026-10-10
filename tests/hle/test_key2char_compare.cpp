// cellKey2char against Sony's libkey2char (D-565, D-597): the same calls on the same handles and key data go to
// pspocket's library and to Sony's module. Compared: every answer, the 128-byte handle the game owns after each call
// that may change it, and for each key GetChar's charNum, processed flag and the characters *charCode points at, in all
// 25 arrangements and the three modes, with shift, caps lock, num lock and the others. Where *charCode points (in the
// handle or elsewhere) is a line of its own, written when it changes. No game data; skipped without the firmware
// (compare_util.hpp).

#include <catch_amalgamated.hpp>

#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "compare_util.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;

namespace {

constexpr u32 kHandle = kData + 0x100, kOther = kData + 0x200, kFresh = kData + 0x300, kKey = kData + 0x400,
              kCharCode = kData + 0x410, kCharNum = kData + 0x414, kProcessed = kData + 0x418;

// The keys typed: letters, digits, Enter to slash, caps lock, F1 and F12, the cursor block, the keypad, the
// 102nd key and the application key, the Japanese keys, the European keys RPCS3's tables add, and codes past them.
std::vector<u16> keys()
{
    std::vector<u16> k;
    for (u16 c = 0x04; c <= 0x39; c++) k.push_back(c);
    for (const u16 c : {0x3A, 0x45, 0x49, 0x4A, 0x4C, 0x4F, 0x50, 0x51, 0x52}) k.push_back(c);
    for (u16 c = 0x53; c <= 0x65; c++) k.push_back(c);
    for (const u16 c : {0x87, 0x88, 0x89, 0x8A, 0x8B, 0x90, 0x91, 0x92, 0x93, 0x01, 0xE0, 0xFF, 0x104}) k.push_back(c);
    return k;
}

// Modifier keys and LEDs (CELL_KB_MKEY_*, CELL_KB_LED_*): none, left and right shift, caps lock with and without
// shift, num lock, the controls, alt and AltGr (right alt), kana.
struct Combo {
    u32 mkey, led;
    const char* name;
};
constexpr Combo kEnglish[] = {{0, 0, "plain"}, {0x02, 0, "shift"}, {0x20, 0, "right shift"}, {0, 0x02, "caps lock"},
    {0x02, 0x02, "shift caps lock"}, {0, 0x01, "num lock"}, {0x01, 0, "ctrl"}, {0x04, 0, "alt"},
    {0x40, 0, "altgr"}, {0, 0x10, "kana"}};
constexpr Combo kNative[] = {{0, 0, "plain"}, {0x02, 0, "shift"}, {0, 0x02, "caps lock"}, {0, 0x10, "kana"}};

struct Run : Half<> {
    u32 last_at = ~0u;

    using Half::Half;

    void handle(const std::string& when, u32 at = kHandle) { o.line("handle after " + when, hex(b + at, 128)); }

    // GetChar on `h` with the key data, then what it wrote: charNum, processed, the characters (charNum of them, at
    // most 8) and, when it changed, where *charCode points.
    void get_char(u32 h, u16 keycode, u32 mkey, u32 led, const std::string& key)
    {
        ps_write32(b, kKey, led);
        ps_write32(b, kKey + 4, mkey);
        ps_write16(b, kKey + 8, keycode);
        ps_write16(b, kKey + 10, 0);
        ps_write32(b, kCharCode, 0);
        ps_write32(b, kCharNum, 0xFFFFFFFF);
        ps_write8(b, kProcessed, 0xEE);
        const std::optional<u64> v = side.call("cellKey2CharGetChar", {h, kKey, kCharCode, kCharNum, kProcessed});
        if (!v) {
            o.line(key, "(not exported)");
            return;
        }
        const u32 at = ps_read32(b, kCharCode), n = ps_read32(b, kCharNum);
        std::string chars;
        if (readable(at, 2 * std::min(n, 8u)))
            for (u32 i = 0; i < std::min(n, 8u); i++)
                chars += std::format("{}{:04x}", i ? " " : "", ps_read16(b, at + 2 * i));
        else if (at && n && n != 0xFFFFFFFF)
            chars = std::format("not in guest memory ({:#x})", at);
        o.line(key, std::format("r {} n {:#x} processed {:#04x} chars [{}]", answer(*v), n, ps_read8(b, kProcessed),
                        chars));
        if (at != last_at) {
            last_at = at;
            const std::string where = !at ? "null"
                : at >= h && at < h + 128 ? std::format("the handle +{:#x}", at - h)
                                          : std::string("outside the handle");
            o.line("*charCode points at (" + key + ")", where);
            if (where == "outside the handle") o.note(std::format("{}: *charCode {:#x}", side.who(), at));
        }
    }
};

Observed half(const Setup& s, bool sony_half)
{
    Run h(s, sony_half, "cellKey2char");
    u8* b = h.b;
    std::memset(b + kHandle, 0, 128);
    std::memset(b + kFresh, 0, 128);

    // A game opens the keyboard library first (pspocket's sys_io in both halves: no keyboard is connected); Open
    // before that, once.
    h.r("cellKey2CharOpen", {kFresh}, "Open before cellKbInit");
    h.r("cellKey2CharClose", {kFresh}, "Close before cellKbInit");
    std::memset(b + kFresh, 0, 128);
    h.o.line("cellKbInit(1)", answer(h.t.call("cellKbInit", 1)));

    // Before Open.
    h.r("cellKey2CharClose", {kHandle}, "Close before Open");
    h.r("cellKey2CharSetMode", {kHandle, 0}, "SetMode before Open");
    h.r("cellKey2CharSetArrangement", {kHandle, 0}, "SetArrangement before Open");
    h.get_char(kHandle, 0x04, 0, 0, "GetChar before Open");
    h.handle("the calls before Open");

    // Open: no handle, a zeroed handle, again, a handle full of garbage, one with only byte 8 clear.
    h.r("cellKey2CharOpen", {0}, "Open(NULL)");
    h.r("cellKey2CharOpen", {kHandle}, "Open");
    h.handle("Open");
    h.r("cellKey2CharOpen", {kHandle}, "Open again");
    h.handle("Open again");
    std::memset(b + kOther, 0xA5, 128);
    h.r("cellKey2CharOpen", {kOther}, "Open of a handle full of 0xA5");
    h.handle("Open of a handle full of 0xA5", kOther);
    ps_write8(b, kOther + 8, 0);
    h.r("cellKey2CharOpen", {kOther}, "Open of a handle of 0xA5 with byte 8 clear");
    h.handle("Open of a handle of 0xA5 with byte 8 clear", kOther);
    h.r("cellKey2CharClose", {kOther}, "Close of that handle");

    // Modes and arrangements, in and out of range.
    for (const s32 mode : {-1, 3, 0x100, 1, 2, 0}) {
        const std::string k = std::format("SetMode({})", mode);
        h.r("cellKey2CharSetMode", {kHandle, static_cast<u64>(static_cast<u32>(mode))}, k);
        h.handle(k);
    }
    for (const s32 arr : {-1, 25, 100, 24, 1, 0}) {
        const std::string k = std::format("SetArrangement({})", arr);
        h.r("cellKey2CharSetArrangement", {kHandle, static_cast<u64>(static_cast<u32>(arr))}, k);
        h.handle(k);
    }
    h.r("cellKey2CharSetMode", {0, 0}, "SetMode(NULL)");
    h.r("cellKey2CharSetArrangement", {0, 0}, "SetArrangement(NULL)");

    // GetChar's checks: no key data, no character pointer, key code 0, a handle never opened; charNum and processed
    // left out.
    ps_write32(b, kKey, 0);
    ps_write32(b, kKey + 4, 0);
    ps_write16(b, kKey + 8, 0x04);
    h.r("cellKey2CharGetChar", {kHandle, 0, kCharCode, kCharNum, kProcessed}, "GetChar without key data");
    h.r("cellKey2CharGetChar", {kHandle, kKey, 0, kCharNum, kProcessed}, "GetChar without charCode");
    h.r("cellKey2CharGetChar", {0, kKey, kCharCode, kCharNum, kProcessed}, "GetChar(NULL handle)");
    h.r("cellKey2CharGetChar", {kFresh, kKey, kCharCode, kCharNum, kProcessed}, "GetChar on a handle never opened");
    ps_write32(b, kCharCode, 0);
    h.r("cellKey2CharGetChar", {kHandle, kKey, kCharCode, 0, 0}, "GetChar without charNum and processed");
    h.o.line("*charCode without charNum and processed",
        ps_read32(b, kCharCode) >= kHandle && ps_read32(b, kCharCode) < kHandle + 128
            ? std::format("the handle +{:#x}", ps_read32(b, kCharCode) - kHandle)
            : std::format("{}", ps_read32(b, kCharCode) ? "outside the handle" : "null"));
    h.get_char(kHandle, 0, 0, 0, "GetChar key code 0");

    // Every key in every arrangement and mode: each (arrangement, mode) on a handle opened afresh.
    const std::vector<u16> all = keys();
    for (s32 arr = 0; arr <= 24; arr++) {
        for (const s32 mode : {0, 1, 2}) {
            const std::string group = std::format("arrangement {} mode {}", arr, mode);
            h.o.group(group);
            h.r("cellKey2CharClose", {kHandle}, group + ": Close");
            h.r("cellKey2CharOpen", {kHandle}, group + ": Open");
            h.r("cellKey2CharSetArrangement", {kHandle, static_cast<u64>(arr)}, group + ": SetArrangement");
            h.r("cellKey2CharSetMode", {kHandle, static_cast<u64>(mode)}, group + ": SetMode");
            h.handle(group + ": SetMode");
            const auto& combos = mode == 0 ? std::vector<Combo>(std::begin(kEnglish), std::end(kEnglish))
                                           : std::vector<Combo>(std::begin(kNative), std::end(kNative));
            for (const Combo& c : combos) {
                for (const u16 key : all)
                    h.get_char(kHandle, key, c.mkey, c.led, std::format("{} {}: key {:#04x}", group, c.name, key));
                h.handle(std::format("{} {}: the keys", group, c.name));
            }
        }
    }
    h.o.group("");

    // A handle marked broken (byte 0 = 255, RPCS3's reading of it).
    h.r("cellKey2CharSetArrangement", {kHandle, 0}, "SetArrangement(0) before marking the handle");
    h.r("cellKey2CharSetMode", {kHandle, 0}, "SetMode(0) before marking the handle");
    const u8 status = ps_read8(b, kHandle);
    ps_write8(b, kHandle, 255);
    h.r("cellKey2CharSetMode", {kHandle, 0}, "SetMode, byte 0 255");
    h.r("cellKey2CharSetArrangement", {kHandle, 0}, "SetArrangement, byte 0 255");
    h.get_char(kHandle, 0x04, 0, 0, "GetChar, byte 0 255");
    ps_write8(b, kHandle, status);

    // Close, and what is left.
    h.r("cellKey2CharClose", {kHandle}, "Close");
    h.handle("Close");
    h.r("cellKey2CharClose", {kHandle}, "Close again");
    h.get_char(kHandle, 0x04, 0, 0, "GetChar after Close");
    h.r("cellKey2CharOpen", {kHandle}, "Open after Close");
    h.handle("Open after Close");
    h.r("cellKey2CharClose", {kHandle}, "the last Close");
    return std::move(h.o);
}

} // namespace

TEST_CASE("cellKey2char's comparison calls Sony's exports by their names; pspocket's library, firmware first",
    "[hle][key2char]")
{
    check_names("cellKey2char", {"cellKey2CharOpen", "cellKey2CharClose", "cellKey2CharGetChar", "cellKey2CharSetMode",
                                    "cellKey2CharSetArrangement"});
}

TEST_CASE("cellKey2char against Sony's libkey2char: answers, the handle, every key in every arrangement and mode",
    "[.][hle][lle][compare][key2char]")
{
    const Setup s = setup();
    run(s, "cellKey2char against Sony's libkey2char", [&](bool sony) { return half(s, sony); },
        [](const Observed& o) {
            // pspocket's own half: the answers and characters its unit tests check (test_key2char.cpp).
            CHECK(o.value("Open") == "0x0");
            CHECK(o.value("Open again") == "0x80121304");
            CHECK(o.value("Close before Open") == "0x80121305");
            CHECK(o.value("SetMode(3)") == "0x80121303");
            CHECK(o.value("arrangement 0 mode 0 plain: key 0x04") == "r 0x0 n 0x1 processed 0x01 chars [0061]");
            CHECK(o.value("arrangement 0 mode 0 shift: key 0x04") == "r 0x0 n 0x1 processed 0x01 chars [0041]");
            CHECK(o.value("arrangement 0 mode 0 plain: key 0x29") == "r 0x0 n 0x0 processed 0x00 chars []");
            CHECK(o.value("arrangement 1 mode 0 plain: key 0x89") == "r 0x0 n 0x1 processed 0x01 chars [00a5]");
            CHECK(o.value("arrangement 3 mode 0 plain: key 0x92") == "r 0x0 n 0x1 processed 0x01 chars [00df]");
            CHECK(o.value("GetChar, byte 0 255") == "r 0x80121306 n 0xffffffff processed 0xee chars []");
            CHECK(o.value("*charCode points at (GetChar before Open)") == "null");
            CHECK(o.lines.size() > 30000);
        });
}
