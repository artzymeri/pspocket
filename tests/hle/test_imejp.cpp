// cellImeJp (synthetic: no game data): the Japanese input method as RPCS3 has it. Opening and closing, characters and
// strings in, the cursor and focus moving as RPCS3's do, conversion with the focus string as the one candidate, and
// confirming (D-188).

#include <catch_amalgamated.hpp>

#include <cstring>
#include <string>
#include <vector>

#include "hle_test_util.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u16 A = 0x3042, I = 0x3044, U = 0x3046, E = 0x3048; // あ い う え

const u64 kErr = r3_of_code(0x8002BF01), kContext = r3_of_code(0x8002BF11), kAlreadyOpen = r3_of_code(0x8002BF21),
          kParam = r3_of_code(0x8002BF41);

// The u16 string at `addr` (up to `max` characters, stopping at a NUL).
std::vector<u16> read_u16s(u8* base, u32 addr, u32 max = 128)
{
    std::vector<u16> v;
    for (u32 i = 0; i < max; i++) {
        const u16 c = ps_read16(base, addr + 2 * i);
        if (!c) break;
        v.push_back(c);
    }
    return v;
}

} // namespace

TEST_CASE("cellImeJp: open and close, input, cursor, focus, conversion and confirmation as RPCS3 (D-188)",
    "[hle][imejp]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* base = f.base();
    constexpr u32 kHandle = kData + 0x100, kOut = kData + 0x104, kCount = kData + 0x108, kStr = kData + 0x200,
                  kBuf = kData + 0x400, kDic = kData + 0x800, kDics = kData + 0xA00, kItem = kData + 0xB00,
                  kUnmapped = 0x0FFF0000;
    const auto status = [&]() -> u16 {
        REQUIRE(a.call("cellImeJpGetStatus", 0, kOut) == 0);
        return ps_read16(base, kOut);
    };
    const auto s16_out = [&](const char* fn) -> s16 {
        REQUIRE(a.call(fn, 0, kOut) == 0);
        return static_cast<s16>(ps_read16(base, kOut));
    };
    const auto convert_string = [&] {
        std::memset(base + kBuf, 0xAA, 256);
        REQUIRE(a.call("cellImeJpGetConvertString", 0, kBuf) == 0);
        return read_u16s(base, kBuf);
    };
    const auto enter = [&](u16 c) {
        ps_write16(base, kOut, 0);
        REQUIRE(a.call("cellImeJpEnterChar", 0, c, kOut) == 0);
        CHECK(ps_read16(base, kOut) == 2); // CELL_IMEJP_RET_CONFIRMED
    };

    // Closed: the pointer is checked first, then the IME.
    CHECK(a.call("cellImeJpGetStatus", 0, 0) == kParam);
    CHECK(a.call("cellImeJpGetStatus", 0, kOut) == kContext);
    CHECK(a.call("cellImeJpClose", 0) == kContext);
    CHECK(a.call("cellImeJpEnterChar", 0, A, kOut) == kContext);

    // Open: a container and a handle pointer; one at a time; the handle is RPCS3's.
    std::memset(base + kDic, 0, 256);
    std::memcpy(base + kDic, "/dev_hdd0/game/TEST00001/USRDIR/user.dic", 41);
    CHECK(a.call("cellImeJpOpen", 0, kHandle, kDic) == kParam);
    CHECK(a.call("cellImeJpOpen", 1, 0, kDic) == kParam);
    REQUIRE(a.call("cellImeJpOpen", 1, kHandle, kDic) == 0);
    CHECK(ps_read32(base, kHandle) == 0xF0000000u);
    CHECK(a.call("cellImeJpOpen2", 1, kHandle, 0) == kAlreadyOpen);
    CHECK(a.call("cellImeJpOpen3", 1, kHandle, 0) == kAlreadyOpen);
    CHECK(status() == 0); // BEFORE_INPUT

    // Input settings: the kana mode and the extension characters only before input.
    CHECK(a.call("cellImeJpSetKanaInputMode", 0, 1) == 0);
    CHECK(a.call("cellImeJpAllowExtensionCharacters", 0, 0) == 0);
    enter(A);
    CHECK(status() == 1); // BEFORE_CONVERT
    CHECK(a.call("cellImeJpSetKanaInputMode", 0, 0) == kErr);
    CHECK(a.call("cellImeJpAllowExtensionCharacters", 0, 0x10) == kErr);
    CHECK(a.call("cellImeJpSetInputCharType", 0, 2) == 0);
    CHECK(a.call("cellImeJpSetFixInputMode", 0, 1) == 0);

    // A string up to its NUL; the focus grows with characters typed inside it. Focus offsets are in bytes.
    ps_write16(base, kStr, I);
    ps_write16(base, kStr + 2, U);
    ps_write16(base, kStr + 4, 0);
    ps_write16(base, kOut, 0);
    REQUIRE(a.call("cellImeJpEnterString", 0, kStr, kOut) == 0);
    CHECK(ps_read16(base, kOut) == 2);
    CHECK(convert_string() == std::vector<u16>{A, I, U});
    for (u32 i = 3; i < 128; i++) CHECK(ps_read16(base, kBuf + 2 * i) == 0);
    CHECK(s16_out("cellImeJpGetFocusTop") == 0);
    CHECK(s16_out("cellImeJpGetFocusLength") == 6);

    // Editing before conversion: the cursor, delete at it, backspace before it.
    REQUIRE(a.call("cellImeJpModeCaretLeft", 0) == 0); // cursor 2
    REQUIRE(a.call("cellImeJpDeleteWord", 0) == 0);    // removes う
    CHECK(convert_string() == std::vector<u16>{A, I});
    CHECK(s16_out("cellImeJpGetFocusLength") == 4);
    REQUIRE(a.call("cellImeJpBackspaceWord", 0) == 0); // removes い, cursor 1
    CHECK(convert_string() == std::vector<u16>{A});
    CHECK(s16_out("cellImeJpGetFocusLength") == 2);
    REQUIRE(a.call("cellImeJpModeCaretRight", 0) == 0); // stays at the end
    enter(E);
    CHECK(convert_string() == std::vector<u16>{A, E});
    CHECK(s16_out("cellImeJpGetFocusLength") == 4);

    // Converting: the focus string is the one candidate.
    CHECK(a.call("cellImeJpGetCandidateListSize", 0, kOut) == kErr);  // not yet
    CHECK(a.call("cellImeJpConvertCancel", 0) == kErr);               // nothing to cancel
    CHECK(a.call("cellImeJpExtendConvertArea", 0) == kErr);
    REQUIRE(a.call("cellImeJpConvertForward", 0) == 0);
    CHECK(status() == 7); // CANDIDATES
    CHECK(a.call("cellImeJpModeCaretLeft", 0) == kErr);
    REQUIRE(a.call("cellImeJpEnterChar", 0, A, kOut) == 0); // allowed while converting (RPCS3): back to editing
    CHECK(status() == 1);
    REQUIRE(a.call("cellImeJpConvertForward", 0) == 0);
    CHECK(s16_out("cellImeJpGetCandidateListSize") == 18); // "あえあ\0" 8, plus offset 2 and "あえあ\0" 8
    CHECK(s16_out("cellImeJpGetCandidateSelect") == 0);
    std::memset(base + kBuf, 0xAA, 64);
    REQUIRE(a.call("cellImeJpGetCandidateList", 0, kCount, kBuf) == 0);
    CHECK(static_cast<s16>(ps_read16(base, kCount)) == 1);
    const std::vector<u16> list = {A, E, A, 0, A, E, A, 0, 0};
    for (u32 i = 0; i < list.size(); i++) CHECK(ps_read16(base, kBuf + 2 * i) == list[i]);
    CHECK(ps_read16(base, kBuf + 2 * static_cast<u32>(list.size())) == 0xAAAA);
    CHECK(a.call("cellImeJpGetCandidateList", 0, 0, kBuf) == kParam);
    CHECK(a.call("cellImeJpTemporalConfirm", 0, 0) == 0);

    // The focus: its end wraps between 1 and the rest of the input; clauses move it.
    REQUIRE(a.call("cellImeJpExtendConvertArea", 0) == 0); // 3 of 3: wraps to 1
    CHECK(s16_out("cellImeJpGetFocusLength") == 2);
    REQUIRE(a.call("cellImeJpShortenConvertArea", 0) == 0); // 1: wraps to 3
    CHECK(s16_out("cellImeJpGetFocusLength") == 6);
    REQUIRE(a.call("cellImeJpMoveFocusClause", 0, 0) == 0); // next: 1..2
    CHECK(status() == 2); // CONVERTING
    CHECK(s16_out("cellImeJpGetFocusTop") == 2);
    CHECK(s16_out("cellImeJpGetFocusLength") == 4);
    REQUIRE(a.call("cellImeJpMoveFocusClause", 0, 2) == 0); // top
    CHECK(s16_out("cellImeJpGetFocusTop") == 0);
    CHECK(s16_out("cellImeJpGetFocusLength") == 2);
    REQUIRE(a.call("cellImeJpMoveFocusClause", 0, 3) == 0); // end: the last character
    CHECK(s16_out("cellImeJpGetFocusTop") == 4);
    CHECK(s16_out("cellImeJpGetFocusLength") == 2);
    REQUIRE(a.call("cellImeJpMoveFocusClause", 0, 1) == 0); // before
    CHECK(s16_out("cellImeJpGetFocusTop") == 2);
    CHECK(a.call("cellImeJpGetCandidateListSize", 0, kOut) == kErr); // converting, not choosing

    // Cancelling the conversion goes back to editing; confirming takes the input as typed.
    REQUIRE(a.call("cellImeJpAllConvertCancel", 0) == 0);
    CHECK(status() == 1);
    CHECK(a.call("cellImeJpAllConvertCancel", 0) == kErr);
    CHECK(a.call("cellImeJpMoveFocusClause", 0, 0) == kErr);
    CHECK(a.call("cellImeJpPostConvert", 0, 1) == 0);
    CHECK(a.call("cellImeJpCurrentPartConfirm", 0, 0) == 0);
    REQUIRE(a.call("cellImeJpAllConfirm", 0) == 0);
    CHECK(status() == 0);
    std::memset(base + kBuf, 0xAA, 256);
    REQUIRE(a.call("cellImeJpGetConfirmString", 0, kBuf) == 0);
    CHECK(read_u16s(base, kBuf) == std::vector<u16>{A, E, A});
    REQUIRE(a.call("cellImeJpGetConfirmYomiString", 0, kBuf) == 0);
    CHECK(read_u16s(base, kBuf) == std::vector<u16>{A, E, A});
    CHECK(convert_string().empty());
    CHECK(a.call("cellImeJpAllConfirm", 0) == kErr);
    CHECK(a.call("cellImeJpAllDeleteConvertString", 0) == kErr);
    CHECK(a.call("cellImeJpConvertForward", 0) == kErr);
    CHECK(a.call("cellImeJpPostConvert", 0, 1) == kErr);
    CHECK(a.call("cellImeJpGetConvertString", 0, kUnmapped) == kParam); // RPCS3 faults

    // At most 127 characters; a NUL character adds nothing.
    enter(0);
    CHECK(status() == 0);
    for (u32 i = 0; i < 130; i++) ps_write16(base, kStr + 2 * i, static_cast<u16>(A + (i % 3)));
    ps_write16(base, kStr + 260, 0);
    REQUIRE(a.call("cellImeJpEnterString", 0, kStr, kOut) == 0);
    CHECK(convert_string().size() == 127);
    REQUIRE(a.call("cellImeJpAllDeleteConvertString", 0) == 0);
    CHECK(status() == 0);
    CHECK(convert_string().empty());

    // No predictions.
    ps_write32(base, kCount, 0xAAAAAAAA);
    REQUIRE(a.call("cellImeJpGetPredictList", 0, 0, 5, kCount, kItem) == 0);
    CHECK(ps_read32(base, kCount) == 0);
    CHECK(a.call("cellImeJpGetPredictList", 0, 0, 5, kCount, 0) == kParam);
    CHECK(a.call("cellImeJpConfirmPrediction", 0, 0) == kParam);
    CHECK(a.call("cellImeJpConfirmPrediction", 0, kItem) == 0);

    // Reset clears the confirmed string too; Close closes, and the IME opens again (Open3 with a list of dictionaries).
    REQUIRE(a.call("cellImeJpReset", 0) == 0);
    REQUIRE(a.call("cellImeJpGetConfirmString", 0, kBuf) == 0);
    CHECK(read_u16s(base, kBuf).empty());
    REQUIRE(a.call("cellImeJpClose", 0) == 0);
    CHECK(a.call("cellImeJpClose", 0) == kContext);
    ps_write32(base, kDics, kDic);
    ps_write32(base, kDics + 4, 0);
    ps_write32(base, kDics + 8, 0);
    ps_write32(base, kDics + 12, 0);
    REQUIRE(a.call("cellImeJpOpen3", 1, kHandle, kDics) == 0);
    CHECK(status() == 0);
    REQUIRE(a.call("cellImeJpClose", 0) == 0);
    REQUIRE(a.call("cellImeJpOpen2", 1, kHandle, 0) == 0);
}
