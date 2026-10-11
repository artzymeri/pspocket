// cellVideoPlayerUtility as a console whose system software has no video player to lend (D-679). Synthetic: no game
// data. Initialize and Finalize bracket the library; Open and StartThumbnail are not supported; with nothing open the
// rest answer CELL_ESTAT; no argument is read or written.

#include <catch_amalgamated.hpp>

#include <cstring>
#include <iterator>
#include <string_view>

#include "hle_test_util.hpp"
#include "pspocket/hle/hle.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

const u64 kStat = r3_of(lv2::CELL_ESTAT), kNotSup = r3_of(lv2::CELL_ENOTSUP);

// What needs the system's player.
constexpr std::string_view kNeedPlayer[] = {"cellVideoPlayerOpen", "cellVideoPlayerStartThumbnail"};
// What needs content open, or a thumbnail started.
constexpr std::string_view kNeedOpen[] = {"cellVideoPlayerClose", "cellVideoPlayerPlaybackControl",
    "cellVideoPlayerGetOutputPicture", "cellVideoPlayerGetOutputStereoPicture", "cellVideoPlayerGetPlaybackStatus",
    "cellVideoPlayerGetTransferPictureInfo", "cellVideoPlayerGetVolume", "cellVideoPlayerSetVolume",
    "cellVideoPlayerSetDownloadPosition", "cellVideoPlayerSetStartPosition", "cellVideoPlayerSetStopPosition",
    "cellVideoPlayerSetTransferComplete", "cellVideoPlayerEndThumbnail"};

// Guest memory the arguments point at, filled with a pattern: no call may change it.
constexpr u32 kArgs = kData + 0x1000, kArgsSize = 0x400;

void fill(u8* base) { std::memset(base + kArgs, 0x5A, kArgsSize); }
bool untouched(u8* base)
{
    for (u32 i = 0; i < kArgsSize; i++)
        if (base[kArgs + i] != 0x5A) return false;
    return true;
}

// A call with every argument register a pointer into the pattern.
u64 call(GuestThread& a, std::string_view fn)
{
    return a.call(fn, kArgs, kArgs + 0x80, kArgs + 0x100, kArgs + 0x180, kArgs + 0x200, kArgs + 0x280, kArgs + 0x300,
        kArgs + 0x380);
}

} // namespace

TEST_CASE("cellVideoPlayerUtility: every stub RPCS3 left is pspocket's own, unverified (D-679)", "[hle][videoplayer]")
{
    usize n = 0;
    for (const hle::StubFunction& s : hle::rpcs3_stubs()) {
        if (s.module != "cellVideoPlayerUtility") continue;
        INFO(s.name);
        const hle::Function* fn = hle::registry().find(s.module, s.nid);
        REQUIRE(fn != nullptr);
        CHECK(fn->name == s.name);
        CHECK(hle::registry().is_unverified(s.module, s.nid));
        n++;
    }
    CHECK(n == 17);
    CHECK(n == std::size(kNeedPlayer) + std::size(kNeedOpen) + 2); // and Initialize, Finalize
}

TEST_CASE("cellVideoPlayerUtility: Initialize and Finalize bracket it; nothing ever opens (D-679)", "[hle][videoplayer]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* base = f.base();
    fill(base);

    // Before Initialize, everything but Initialize is CELL_ESTAT.
    CHECK(call(a, "cellVideoPlayerFinalize") == kStat);
    for (const std::string_view fn : kNeedPlayer) {
        INFO(fn);
        CHECK(call(a, fn) == kStat);
    }
    for (const std::string_view fn : kNeedOpen) {
        INFO(fn);
        CHECK(call(a, fn) == kStat);
    }

    // Initialized once.
    REQUIRE(call(a, "cellVideoPlayerInitialize") == 0);
    CHECK(call(a, "cellVideoPlayerInitialize") == kStat);

    // No player: nothing opens and no thumbnail starts; with nothing open the rest refuse, as often as they are called
    // (a player is polled every frame).
    for (int round = 0; round < 3; round++) {
        for (const std::string_view fn : kNeedPlayer) {
            INFO(fn);
            CHECK(call(a, fn) == kNotSup);
        }
        for (const std::string_view fn : kNeedOpen) {
            INFO(fn);
            CHECK(call(a, fn) == kStat);
        }
    }
    CHECK(untouched(base));

    // Finalized: back to before Initialize; initialized again, the same.
    REQUIRE(call(a, "cellVideoPlayerFinalize") == 0);
    CHECK(call(a, "cellVideoPlayerFinalize") == kStat);
    CHECK(call(a, "cellVideoPlayerOpen") == kStat);
    REQUIRE(call(a, "cellVideoPlayerInitialize") == 0);
    CHECK(call(a, "cellVideoPlayerOpen") == kNotSup);
    CHECK(call(a, "cellVideoPlayerGetPlaybackStatus") == kStat);
    CHECK(untouched(base));
}

TEST_CASE("cellVideoPlayerUtility: a process each (D-679)", "[hle][videoplayer]")
{
    {
        Fixture f;
        GuestThread a(f, 0, "a");
        REQUIRE(a.call("cellVideoPlayerInitialize") == 0);
    }
    Fixture f;
    GuestThread a(f, 0, "a");
    CHECK(a.call("cellVideoPlayerFinalize") == kStat); // the last process's Initialize is not this one's
    REQUIRE(a.call("cellVideoPlayerInitialize") == 0);
}
