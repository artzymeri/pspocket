// cellKey2char against Sony's libkey2char (D-565, D-597, D-914): the same calls on the same handles and key data go to
// pspocket's library and to Sony's module. Compared: every answer, the 128-byte handle the game owns after each call
// that may change it (libkey2char keeps all its state there, the Korean automaton's stacks too), and for each key
// GetChar's charNum, processed flag and the characters *charCode points at: every key in all 25 arrangements and the
// three modes, with shift, caps lock, num lock and the others; Korean, Japanese and dead-key typing in sequences;
// and the module's nine other exports (its Hangul automaton's functions, known by NID) on an automaton of the test's.
// Where *charCode points is a line of its own, written when it changes. charNum and processed are never NULL (the
// module writes them unchecked). No game data; skipped without the firmware (compare_util.hpp). In the gate since
// D-914.

#include <catch_amalgamated.hpp>

#include <cstring>
#include <format>
#include <iterator>
#include <random>
#include <utility>
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
constexpr Combo kNative[] = {{0, 0, "plain"}, {0x02, 0, "shift"}, {0, 0x02, "caps lock"},
    {0x02, 0x02, "shift caps lock"}, {0, 0x01, "num lock"}, {0, 0x10, "kana"}};

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
        trace("calling: " + key);
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
    // charNum and processed are not optional: libkey2char writes both without a check (a NULL one faults on a PS3 as
    // it did here, at guest address 0), so they are not left out.
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

    // Typing in a native mode: words, backspaces and keys that are no letters, each key's characters and the handle
    // (the Korean syllable being composed lives in it) after it.
    const auto type_keys = [&](const std::string& what, u32 arr, u32 mode,
                               std::initializer_list<std::pair<u16, u32>> keys, u32 led = 0) {
        h.o.group(what);
        h.r("cellKey2CharClose", {kHandle}, what + ": Close");
        h.r("cellKey2CharOpen", {kHandle}, what + ": Open");
        h.r("cellKey2CharSetArrangement", {kHandle, arr}, what + ": SetArrangement");
        h.r("cellKey2CharSetMode", {kHandle, mode}, what + ": SetMode");
        u32 i = 0;
        for (const auto& [key, mkey] : keys) {
            const std::string k = std::format("{} #{}: key {:#04x} mkey {:#x}", what, i++, key, mkey);
            h.get_char(kHandle, key, mkey, led, k);
            h.handle(k);
        }
        h.o.group("");
    };
    constexpr u32 S = 0x02; // shift
    // Korean: 안녕하세요 (dk ss ud gk tp dy), 한글 (gks rmf), a final moving on, compound medials and finals, the
    // doubled consonants, backspaces through a syllable and past it, keys that are no jamo in between.
    type_keys("korean words", 11, 1,
        {{0x07, 0}, {0x0E, 0}, {0x16, 0}, {0x16, 0}, {0x18, 0}, {0x07, 0}, {0x0A, 0}, {0x0E, 0}, {0x17, 0}, {0x13, 0},
            {0x07, 0}, {0x1C, 0}, {0x2C, 0}, {0x0A, 0}, {0x0E, 0}, {0x16, 0}, {0x15, 0}, {0x10, 0}, {0x09, 0},
            {0x2A, 0}, {0x2A, 0}, {0x2A, 0}, {0x2A, 0}, {0x2A, 0}, {0x09, 0}, {0x15, 0}, {0x0E, 0}, {0x0F, 0},
            {0x0B, 0}, {0x0E, 0}, {0x0F, 0}, {0x11, 0}, {0x0D, 0}, {0x0F, 0}, {0x10, 0}, {0x0F, 0}, {0x0E, 0},
            {0x09, 0}, {0x15, 0}, {0x17, 0}, {0x0E, 0}, {0x14, 0}, {0x17, 0}, {0x0D, 0}, {0x16, 0}, {0x1A, 0},
            {0x0E, 0}, {0x16, 0}, {0x0A, 0}, {0x0E, 0}, {0x14, S}, {0x0E, 0}, {0x1A, S}, {0x0E, 0}, {0x08, S},
            {0x0E, 0}, {0x15, S}, {0x17, S}, {0x0E, 0}, {0x15, S}, {0x12, S}, {0x13, S}, {0x1E, 0}, {0x0E, 0},
            {0x29, 0}, {0x0E, 0}, {0x28, 0}, {0x0E, 0}, {0x59, 0}, {0x0E, 0}, {0x3A, 0}, {0x2A, 0}, {0x0E, 0},
            {0x15, 0}, {0x2A, 0}, {0x2A, 0}, {0x2A, 0}, {0x2A, 0}, {0x04, S}, {0x33, S}, {0x104, 0}, {0x90, 0}});
    type_keys("korean with num lock", 11, 1, {{0x0E, 0}, {0x59, 0}, {0x0E, 0}, {0x63, 0}, {0x54, 0}, {0x16, 0}},
        0x01);
    // Japanese: kana then dakuten and handakuten, on kana that take them and those that do not, twice, shifted.
    type_keys("kana marks", 1, 1,
        {{0x17, 0}, {0x2F, 0}, {0x2F, 0}, {0x09, 0}, {0x30, 0}, {0x09, 0}, {0x2F, 0}, {0x20, 0}, {0x2F, 0},
            {0x20, 0}, {0x30, 0}, {0x1F, 0}, {0x30, S}, {0x1F, 0}, {0x2F, S}, {0x2E, 0}, {0x30, 0}, {0x1D, S},
            {0x2F, 0}, {0x27, S}, {0x30, 0}, {0x2D, 0}, {0x2F, 0}, {0x2F, 0}, {0x30, 0}, {0x04, 0}, {0x2F, 0},
            {0x1E, 0}, {0x2F, 0}, {0x89, 0}, {0x30, 0}, {0x2C, 0}, {0x30, 0}, {0x2A, 0}, {0x2F, 0}});
    // A kana mark's state kept into the English mode (SetMode does not clear it).
    h.o.group("kana state");
    h.r("cellKey2CharSetMode", {kHandle, 1}, "kana state: SetMode(1)");
    h.get_char(kHandle, 0x2F, 0, 0, "kana state: a dakuten");
    h.r("cellKey2CharSetMode", {kHandle, 0}, "kana state: SetMode(0)");
    for (const u16 key : {0x35, 0x04, 0x2F})
        h.get_char(kHandle, key, 0, 0, std::format("kana state: key {:#04x}", key));
    h.handle("kana state: the keys");
    h.o.group("");
    // Dead keys in every arrangement: each followed by letters it composes with, others, a space, a digit,
    // Backspace and another dead key.
    const std::pair<u16, u32> dead_keys[] = {{0x35, 0}, {0x35, S}, {0x23, S}, {0x2E, 0}, {0x2E, S}, {0x2F, 0},
        {0x2F, S}, {0x30, 0}, {0x34, 0}, {0x34, S}};
    const std::pair<u16, u32> letters[] = {{0x04, 0}, {0x08, 0}, {0x0C, 0}, {0x12, 0}, {0x18, 0}, {0x1C, 0},
        {0x11, 0}, {0x06, 0}, {0x04, S}, {0x08, S}, {0x0C, S}, {0x12, S}, {0x18, S}, {0x1C, S}, {0x11, S}, {0x06, S},
        {0x1B, 0}, {0x1D, 0}, {0x16, 0}, {0x0F, 0}, {0x1B, S}, {0x1D, S}, {0x16, S}, {0x0F, S}, {0x05, 0},
        {0x2C, 0}, {0x1E, 0}, {0x2A, 0}, {0x35, 0}, {0x35, S}};
    for (u32 arr = 0; arr <= 24; arr++) {
        const std::string group = std::format("dead keys, arrangement {}", arr);
        h.o.group(group);
        h.r("cellKey2CharSetArrangement", {kHandle, arr}, group + ": SetArrangement");
        for (const auto& [dk, dm] : dead_keys)
            for (const auto& [lk, lm] : letters) {
                const std::string k = std::format("{}: {:#04x}/{:#x} then {:#04x}/{:#x}", group, dk, dm, lk, lm);
                h.get_char(kHandle, dk, dm, 0, k + ": the first");
                h.get_char(kHandle, lk, lm, 0, k + ": the second");
            }
        // AltGr (right alt) with ^ and `.
        for (const u32 m : {0x40u, 0x42u}) {
            h.get_char(kHandle, 0x23, m, 0, std::format("{}: ^ with mkey {:#x}", group, m));
            h.get_char(kHandle, 0x04, 0, 0, std::format("{}: then a with mkey {:#x}", group, m));
        }
        h.handle(group + ": the keys");
    }
    h.o.group("");

    // The code pages and every dead key's compositions, whatever cellKbCnvRawCode gives in a layout: the handle (the
    // game's memory) set to a dead key waiting (byte 2 = 1, byte 4 the character). Backspace then hands back the
    // waiting character and the backspace as they are: every byte through each arrangement's code page.
    const auto conversion = [&](u32 arr, u32 mode) {
        h.r("cellKey2CharSetArrangement", {kHandle, arr}, std::format("code page {}: SetArrangement", arr));
        h.r("cellKey2CharSetMode", {kHandle, mode}, std::format("code page {}: SetMode({})", arr, mode));
    };
    for (const auto& [arr, mode] : std::initializer_list<std::pair<u32, u32>>{{0, 0}, {3, 0}, {9, 1}, {22, 0}, {24, 0},
             {7, 0}}) {
        const std::string group = std::format("code page {}", arr);
        h.o.group(group);
        conversion(arr, mode);
        for (u32 c = 1; c <= 0xFF; c++) {
            ps_write8(b, kHandle + 2, 1);
            ps_write8(b, kHandle + 4, static_cast<u8>(c));
            h.get_char(kHandle, 0x2A, 0, 0, std::format("{}: byte {:#04x} waiting, then Backspace", group, c));
        }
        // Each dead key with every letter, space and a digit.
        for (const u8 dead : {u8{'`'}, u8{0xB4}, u8{'^'}, u8{'~'}, u8{0xA8}, u8{0xB8}, u8{'a'}})
            for (u16 key = 0x04; key <= 0x2C; key++) {
                if (key > 0x1E && key < 0x2C) continue;
                for (const u32 m : {0u, S}) {
                    ps_write8(b, kHandle + 2, 1);
                    ps_write8(b, kHandle + 4, dead);
                    h.get_char(kHandle, key, m, 0,
                        std::format("{}: dead key {:#04x}, then key {:#04x}/{:#x}", group, dead, key, m));
                }
            }
        h.handle(group + ": the keys");
    }
    h.o.group("");

    // libkey2char's other exports: the Hangul automaton's functions, known only by their NIDs (pspocket names them
    // cellKey2char_<NID>), on an automaton of the test's.
    h.o.group("the automaton's functions");
    const auto nid = [&](u32 id, std::initializer_list<u64> args, const std::string& key) {
        trace("calling: " + key);
        const std::optional<u64> v = h.side.call_nid(id, args);
        h.o.line(key, v ? answer(*v) : std::string("(not exported)"));
        return v ? *v : ~u64{0};
    };
    constexpr u32 kToCompat = 0xEAE114F6, kCompose = 0x28893D81, kInit = 0x83E91458, kClose = 0xAE4B34E6,
                  kRecompute = 0xE5D1F9B0, kRestore = 0x291ADBA5, kBackspace = 0x1B5F6EFC, kAdd = 0x51667234,
                  kReset = 0x7F8E6F3E;
    for (u32 c = 0x10FE; c <= 0x1202; c++) nid(kToCompat, {c}, std::format("to compatibility jamo({:#x})", c));
    for (const u32 c : {0u, 0x41u, 0x3131u, 0xAC00u, 0xFFFFu, 0x11100u, 0x111A8u, 0xFFFF1161u})
        nid(kToCompat, {c}, std::format("to compatibility jamo({:#x})", c));
    for (const u32 l : {0x1100u, 0x1101u, 0x1112u, 0x1113u, 0x115Fu, 0x1160u, 0x10FFu})
        for (const u32 v : {0x1160u, 0x1161u, 0x1175u, 0x1176u, 0x115Fu, 0x11A7u})
            for (const u32 t : {0x11A6u, 0x11A7u, 0x11A8u, 0x11C2u, 0x11C3u, 0x11FFu})
                nid(kCompose, {l, v, t}, std::format("compose({:#x}, {:#x}, {:#x})", l, v, t));
    constexpr u32 kAuto = kData + 0x800;
    const auto automaton = [&](const std::string& when) { h.o.line("the automaton after " + when, hex(b + kAuto, 0x60)); };
    std::memset(b + kAuto, 0xA5, 0x60);
    nid(kClose, {0}, "close(NULL)");
    nid(kInit, {kAuto}, "init");
    automaton("init");
    // Jamo of every kind (and characters that are none) in a made-up order, with backspaces, the stack put back,
    // recomputed, reset, closed and set up again.
    std::mt19937 rng(20261011);
    const auto pick = [&] {
        const u32 r = rng() % 100;
        if (r < 30) return 0x1100 + rng() % 19;
        if (r < 60) return 0x1161 + rng() % 21;
        if (r < 80) return 0x11A8 + rng() % 27;
        static constexpr u32 kOthers[] = {0x115F, 0x1160, 0x11A7, 0x3131, 0x314F, 0xAC00, 0x41, 0x1113, 0x1176,
            0x11C3, 0};
        return kOthers[rng() % std::size(kOthers)];
    };
    for (u32 step = 0; step < 600; step++) {
        const u32 r = rng() % 100;
        std::string key;
        if (r < 75) {
            const u32 j = pick();
            key = std::format("#{}: add({:#x})", step, j);
            nid(kAdd, {kAuto, j}, key);
        } else if (r < 85) {
            key = std::format("#{}: backspace", step);
            nid(kBackspace, {kAuto}, key);
        } else if (r < 90) {
            key = std::format("#{}: restore", step);
            nid(kRestore, {kAuto}, key);
        } else if (r < 94) {
            key = std::format("#{}: recompute", step);
            nid(kRecompute, {kAuto}, key);
        } else if (r < 97) {
            key = std::format("#{}: reset", step);
            nid(kReset, {kAuto}, key);
        } else if (r < 99) {
            key = std::format("#{}: close", step);
            nid(kClose, {kAuto}, key);
        } else {
            key = std::format("#{}: init", step);
            nid(kInit, {kAuto}, key);
        }
        automaton(key);
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

TEST_CASE("cellKey2char's comparison calls Sony's exports by their names; pspocket's library, its own (D-914)",
    "[hle][key2char]")
{
    check_names("cellKey2char", {"cellKey2CharOpen", "cellKey2CharClose", "cellKey2CharGetChar", "cellKey2CharSetMode",
                                    "cellKey2CharSetArrangement"},
        /*marked=*/false);
    // The nine known only by their NIDs.
    for (const u32 nid : {0x83E91458u, 0xAE4B34E6u, 0xE5D1F9B0u, 0x291ADBA5u, 0x1B5F6EFCu, 0x51667234u, 0x7F8E6F3Eu,
             0x28893D81u, 0xEAE114F6u}) {
        const hle::Function* f = hle::registry().find("cellKey2char", nid);
        REQUIRE(f != nullptr);
        CHECK(f->name == std::format("cellKey2char_{:08X}", nid));
    }
}

TEST_CASE("cellKey2char against Sony's libkey2char: answers, the handle, every key in every arrangement and mode",
    "[hle][lle][compare][key2char]")
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
            // libkey2char reads cellKbCnvRawCode's characters past ASCII as Windows-1252: pspocket's sys_io gives the
            // yen sign and sharp s in code page 850 (190 and 225), which read as ¾ and á (as with Sony's module).
            CHECK(o.value("arrangement 1 mode 0 plain: key 0x89") == "r 0x0 n 0x1 processed 0x01 chars [00be]");
            CHECK(o.value("arrangement 3 mode 0 plain: key 0x92") == "r 0x0 n 0x1 processed 0x01 chars [00e1]");
            // SetArrangement does not look at the mark, and sets byte 0 again: the handle converts.
            CHECK(o.value("SetMode, byte 0 255") == "0x80121306");
            CHECK(o.value("GetChar, byte 0 255") == "r 0x0 n 0x1 processed 0x01 chars [0061]");
            CHECK(o.value("korean words #4: key 0x18 mkey 0x0") == "r 0x0 n 0x2 processed 0x01 chars [0008 b140]");
            CHECK(o.value("code page 22: dead key 0x7e, then key 0x04/0x0") == "r 0x0 n 0x1 processed 0x01 chars [0105]");
            CHECK(o.value("*charCode points at (GetChar before Open)") == "null");
            CHECK(o.lines.size() > 30000);
        });
}
