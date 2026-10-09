// The media library utilities (synthetic: no game data): cellSearch over an empty library, cellMusic and
// cellMusicDecode with nothing selected, and the music, photo and video exports, the photo import and the photo decode
// on a temporary /dev_hdd0 (D-186).

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "hle_test_util.hpp"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::ppu::as;
namespace fsys = std::filesystem;

namespace {

struct InterpretMisses {
    const ps_ppu_miss_policy saved = ps_ppu_get_miss_policy();
    InterpretMisses() { ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET); }
    ~InterpretMisses() { ps_ppu_set_miss_policy(saved); }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

constexpr u32 kUnmapped = 0x0FFF0000; // in the main area, never committed by these tests

// A guest callback that logs the low words of r3..r6 (kLog: the count, then 24 bytes an entry), and the two words at
// r5 when `deref` and r5 isn't 0 (cellSearch's CellSearchResultParam, gone once the callback returns).
constexpr u32 kCode = kData + 0x20000, kOpd = kData + 0x20800, kLog = kData + 0x21000;

void write_callback(u8* base, bool deref)
{
    std::vector<u32> code = {
        as::lis(7, static_cast<s32>(kLog >> 16)),
        as::ori(7, 7, kLog & 0xFFFF),
        as::lwz(8, 0, 7),
        as::mulli(9, 8, 24),
        as::addi(9, 9, 16),
        as::add(9, 9, 7),
        as::stw(3, 0, 9),
        as::stw(4, 4, 9),
        as::stw(5, 8, 9),
        as::stw(6, 12, 9),
        as::li(10, 0),
        as::stw(10, 16, 9),
        as::stw(10, 20, 9),
    };
    if (deref) {
        const std::vector<u32> load = {
            as::cmpwi(0, 5, 0),
            as::bc_rel(12, 2, 20), // beq past the loads
            as::lwz(10, 0, 5),
            as::stw(10, 16, 9),
            as::lwz(10, 4, 5),
            as::stw(10, 20, 9),
        };
        code.insert(code.end(), load.begin(), load.end());
    }
    code.insert(code.end(), {as::addi(8, 8, 1), as::stw(8, 0, 7), as::blr()});
    for (usize i = 0; i < code.size(); i++) ps_write32(base, kCode + 4 * static_cast<u32>(i), code[i]);
    ps_write32(base, kOpd, kCode);
    ps_write32(base, kOpd + 4, 0x1234);
    std::memset(base + kLog, 0, 0x800);
}

struct Entry {
    u32 r3, r4, r5, r6, p0, p1;
    bool operator==(const Entry&) const = default;
};

// The callbacks run by a cellSysutilCheckCallback, the log then cleared. Registers past the callback's `args`
// arguments hold whatever was there before, so they read as 0.
std::vector<Entry> take(GuestThread& g, u8* base, u32 args)
{
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    std::vector<Entry> v;
    for (u32 i = 0; i < ps_read32(base, kLog); i++) {
        const u32 at = kLog + 16 + 24 * i;
        const auto reg = [&](u32 n) { return n < args ? ps_read32(base, at + 4 * n) : 0u; };
        v.push_back({reg(0), reg(1), reg(2), reg(3), ps_read32(base, at + 16), ps_read32(base, at + 20)});
    }
    std::memset(base + kLog, 0, 0x800);
    return v;
}

void put(u8* base, u32 addr, std::string_view s)
{
    std::memcpy(base + addr, s.data(), s.size());
    base[addr + s.size()] = 0;
}

bool all_bytes(const u8* p, usize n, u8 value)
{
    for (usize i = 0; i < n; i++)
        if (p[i] != value) return false;
    return true;
}

// A temporary host directory for /dev_hdd0, removed afterwards.
struct TempHdd {
    fsys::path root;
    explicit TempHdd(std::string_view name)
        : root(fsys::temp_directory_path() / std::format("pspocket-media-{}-{}", name, ::getpid()))
    {
        fsys::remove_all(root);
        fsys::create_directories(root);
    }
    ~TempHdd()
    {
        std::error_code ec;
        fsys::remove_all(root, ec);
    }
    TempHdd(const TempHdd&) = delete;
    TempHdd& operator=(const TempHdd&) = delete;

    void write(const fsys::path& rel, std::string_view text) const
    {
        fsys::create_directories((root / rel).parent_path());
        std::ofstream(root / rel, std::ios::binary) << text;
    }
    std::string read(const fsys::path& rel) const
    {
        std::ifstream in(root / rel, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
};

u64 code(u32 c) { return r3_of_code(c); }

} // namespace

TEST_CASE("cellSearch: RPCS3's checks and states over an empty media library (D-186)", "[hle][media][search]")
{
    InterpretMisses interpret;
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* base = f.base();
    write_callback(base, true);
    constexpr u32 kOut = kData + 0x100, kType = kData + 0x110, kOffset = kData + 0x120, kShare = kData + 0x130,
                  kId = kData + 0x140, kTime = kData + 0x160, kTitle = kData + 0x200, kTags = kData + 0x300,
                  kTag = kData + 0x400, kText = kData + 0x500, kPath = kData + 0x600, kBuf = kData + 0x1000,
                  kCtx = kData + 0x2000;
    constexpr u32 kUser = 0x77;
    const u64 kParam = code(0x8002C801), kBusy = code(0x8002C802),
              kMode = code(0x8002C804), kAlready = code(0x8002C805), kNotInit = code(0x8002C806),
              kFinalizing = code(0x8002C807), kNotSupported = code(0x8002C808), kNotFound = code(0x8002C80A),
              kRange = code(0x8002C80C), kBadId = code(0x8002C80D), kGotResult = code(0x8002C80E),
              kTag_ = code(0x8002C812);

    // Nothing before cellSearchInitialize; the output is cleared first.
    ps_write32(base, kOut, 0xAAAAAAAA);
    CHECK(a.call("cellSearchStartListSearch", 1, 1, kOut) == kNotInit);
    CHECK(ps_read32(base, kOut) == 0);
    CHECK(a.call("cellSearchFinalize") == kNotInit);
    CHECK(a.call("cellSearchStartListSearch", 1, 1, kUnmapped) == kParam); // RPCS3 faults

    // Initialize: the mode, the callback; it is busy until the game's next check, which reports the result.
    CHECK(a.call("cellSearchInitialize", 1, 0xFFFFFFFE, kOpd, kUser) == kMode);
    CHECK(a.call("cellSearchInitialize", 0, 0xFFFFFFFE, 0, kUser) == kParam);
    REQUIRE(a.call("cellSearchInitialize", 0, 0xFFFFFFFE, kOpd, kUser) == 0);
    CHECK(a.call("cellSearchInitialize", 0, 0xFFFFFFFE, kOpd, kUser) == kBusy);
    CHECK(a.call("cellSearchStartListSearch", 1, 1, kOut) == kNotInit);
    CHECK(take(a, base, 4) == std::vector<Entry>{{1, 0, 0, kUser, 0, 0}});
    CHECK(a.call("cellSearchInitialize", 0, 0xFFFFFFFE, kOpd, kUser) == kAlready);

    // A list search: its type and ascending order; IDs from 1; busy until the result, which finds nothing.
    CHECK(a.call("cellSearchStartListSearch", 0, 1, kOut) == kParam);
    CHECK(a.call("cellSearchStartListSearch", 10, 1, kOut) == kParam);
    CHECK(a.call("cellSearchStartListSearch", 1, 0, kOut) == kParam);
    CHECK(a.call("cellSearchStartListSearch", 1, 2, kOut) == kNotSupported);
    CHECK(a.call("cellSearchStartListSearch", 1, 1, 0) == kParam);
    REQUIRE(a.call("cellSearchStartListSearch", 9, 1, kOut) == 0);
    CHECK(ps_read32(base, kOut) == 1);
    CHECK(a.call("cellSearchStartListSearch", 1, 1, kOut) == kBusy);
    CHECK(a.call("cellSearchGetContentInfoByOffset", 1, 0, kBuf, kType, kId) == kBusy);
    CHECK(a.call("cellSearchEnd", 1) == kBusy);
    CHECK(a.call("cellSearchFinalize") == kBusy);
    CHECK(a.call("cellSearchCancel", 1) == 0); // RPCS3's TODO: the search still reports
    std::vector<Entry> e = take(a, base, 4);
    REQUIRE(e.size() == 1);
    CHECK(e[0].r3 == 3);
    CHECK(e[0].r4 == 0);
    CHECK(e[0].r5 != 0);
    CHECK(e[0].r6 == kUser);
    CHECK(e[0].p0 == 1); // searchId
    CHECK(e[0].p1 == 0); // resultNum
    CHECK(a.call("cellSearchCancel", 1) == kGotResult);
    CHECK(a.call("cellSearchCancel", 2) == kBadId);

    // Nothing to read back: every offset is out of range, every content ID unknown; outputs cleared first.
    std::memset(base + kBuf, 0xAA, 2048);
    ps_write32(base, kType, 0xAAAAAAAA);
    std::memset(base + kId, 0xAA, 16);
    CHECK(a.call("cellSearchGetContentInfoByOffset", 1, 0, kBuf, kType, kId) == kRange);
    CHECK(all_bytes(base + kBuf, 2048, 0));
    CHECK(ps_read32(base, kType) == 0);
    CHECK(all_bytes(base + kId, 4, 0));
    CHECK(all_bytes(base + kId + 4, 12, 0xFF));
    CHECK(a.call("cellSearchGetContentInfoByOffset", 2, 0, kBuf, kType, kId) == kBadId);
    CHECK(a.call("cellSearchGetContentInfoByOffset", 1, 0, 0, kType, 0) == kParam);
    CHECK(a.call("cellSearchGetContentInfoByOffset", 1, 0, kBuf, 0, kId) == kParam);
    CHECK(a.call("cellSearchGetContentInfoByOffset", 1, 0, kUnmapped, kType, kId) == kParam); // RPCS3 faults
    std::memset(base + kTime, 0, 24);
    CHECK(a.call("cellSearchGetContentIdByOffset", 1, 0, kType, kId, kTime) == kRange);
    CHECK(all_bytes(base + kTime, 24, 0xFF)); // three dates of -1
    CHECK(a.call("cellSearchGetContentIdByOffset", 1, 0, 0, kId, kTime) == kParam);
    CHECK(a.call("cellSearchGetContentIdByOffset", 5, 0, kType, kId, kTime) == kBadId);
    ps_write32(base, kOffset, 0);
    CHECK(a.call("cellSearchGetOffsetByContentId", 1, kId, kOffset) == kNotFound);
    CHECK(ps_read32(base, kOffset) == 0xFFFFFFFFu);
    CHECK(a.call("cellSearchGetOffsetByContentId", 3, kId, kOffset) == kBadId);
    CHECK(a.call("cellSearchGetOffsetByContentId", 1, 0, kOffset) == kParam);
    CHECK(a.call("cellSearchGetContentInfoByContentId", kId, kBuf, kType) == kNotFound);
    CHECK(a.call("cellSearchGetContentInfoByContentId", 0, kBuf, kType) == kParam);
    put(base, kText, "xyz");
    CHECK(a.call("cellSearchGetContentInfoGameComment", kId, kText) == kNotFound);
    CHECK(base[kText] == 0);
    put(base, kText, "xyz");
    CHECK(a.call("cellSearchGetContentInfoDeveloperData", kId, kText) == kNotFound);
    CHECK(base[kText] == 0);
    CHECK(a.call("cellSearchGetContentInfoDeveloperData", 0, kText) == kParam);
    std::memset(base + kPath, 0xAA, 128);
    CHECK(a.call("cellSearchGetContentInfoPath", kId, kPath) == kNotFound);
    CHECK(all_bytes(base + kPath, 128, 0));
    std::memset(base + kPath, 0xAA, 128);
    CHECK(a.call("cellSearchGetContentInfoPathMovieThumb", kId, kPath) == kNotFound);
    CHECK(all_bytes(base + kPath, 128, 0));
    ps_write32(base, kShare, 0xAAAAAAAA);
    CHECK(a.call("cellSearchGetContentInfoSharable", kId, kShare) == kNotFound);
    CHECK(ps_read32(base, kShare) == 0); // PROHIBITED
    std::memset(base + kCtx, 0xAA, 8);
    CHECK(a.call("cellSearchGetMusicSelectionContext", 1, 0, 0, 0, kCtx) == kNotFound);
    CHECK(all_bytes(base + kCtx, 4, 0));
    CHECK(base[kCtx + 4] == 0xAA);
    CHECK(a.call("cellSearchGetMusicSelectionContext", 1, 0, 0, 0, 0) == kParam);
    CHECK(a.call("cellSearchGetMusicSelectionContext", 4, 0, 0, 0, kCtx) == kBadId);
    CHECK(a.call("cellSearchGetMusicSelectionContextOfSingleTrack", kId, kCtx) == kNotFound);
    CHECK(a.call("cellSearchPrepareFile", 0) == kParam);
    put(base, kPath, "/dev_hdd0/.tmp/123song.mp3");
    CHECK(a.call("cellSearchPrepareFile", kPath) == 0);

    // A content search: the sort key and order, the type; then RPCS3's supported orders. A refused one leaves the
    // library idle (RPCS3 leaves it busy).
    CHECK(a.call("cellSearchStartContentSearch", 1, 0, 1, kOut) == kParam);
    CHECK(a.call("cellSearchStartContentSearch", 1, 11, 1, kOut) == kParam);
    CHECK(a.call("cellSearchStartContentSearch", 1, 1, 3, kOut) == kParam);
    CHECK(a.call("cellSearchStartContentSearch", 4, 1, 1, kOut) == kParam);
    CHECK(a.call("cellSearchStartContentSearch", 1, 2, 2, kOut) == kNotSupported); // music: ascending only
    CHECK(a.call("cellSearchStartContentSearch", 1, 8, 1, kOut) == kNotSupported); // music by date taken
    CHECK(a.call("cellSearchStartContentSearch", 2, 2, 2, kOut) == kNotSupported); // photos by title, descending
    CHECK(a.call("cellSearchStartContentSearch", 3, 3, 1, kOut) == kNotSupported); // videos by title only
    REQUIRE(a.call("cellSearchStartContentSearch", 1, 1, 1, kOut) == 0); // music by default: artist
    CHECK(ps_read32(base, kOut) == 2);
    e = take(a, base, 4);
    REQUIRE(e.size() == 1);
    CHECK(e[0].r3 == 5);
    CHECK(e[0].p0 == 2);
    CHECK(e[0].p1 == 0);
    REQUIRE(a.call("cellSearchStartContentSearch", 2, 8, 2, kOut) == 0); // photos by date taken, either order
    CHECK(take(a, base, 4).size() == 1);
    REQUIRE(a.call("cellSearchStartContentSearch", 3, 6, 2, kOut) == 0); // imported date: any order, any type
    CHECK(take(a, base, 4).size() == 1);
    CHECK(ps_read32(base, kOut) == 4);

    // In a list or a video: nothing was ever found to search in; the library stays idle.
    CHECK(a.call("cellSearchStartContentSearchInList", 0, 1, 1, kOut) == kParam);
    CHECK(a.call("cellSearchStartContentSearchInList", kId, 0, 1, kOut) == kParam);
    CHECK(a.call("cellSearchStartContentSearchInList", kId, 1, 0, kOut) == kParam);
    CHECK(a.call("cellSearchStartContentSearchInList", kId, 1, 1, kOut) == kNotFound);
    CHECK(a.call("cellSearchStartSceneSearchInVideo", 0, 1, 1, kOut) == kParam);
    CHECK(a.call("cellSearchStartSceneSearchInVideo", kId, 6, 1, kOut) == kParam);
    CHECK(a.call("cellSearchStartSceneSearchInVideo", kId, 1, 1, kOut) == kNotFound);

    // A scene search: a game title, the type and key, up to six tags each ending within 63 bytes, a supported sort.
    put(base, kTitle, "A Game");
    put(base, kTag, "tag");
    ps_write32(base, kTags, kTag);
    CHECK(a.call("cellSearchStartSceneSearch", 1, 0, 0, 0, 1, 1, kOut) == kParam);
    CHECK(a.call("cellSearchStartSceneSearch", 0, kTitle, 0, 0, 1, 1, kOut) == kParam);
    CHECK(a.call("cellSearchStartSceneSearch", 1, kTitle, 0, 0, 0, 1, kOut) == kParam);
    CHECK(a.call("cellSearchStartSceneSearch", 1, kTitle, kTags, 7, 1, 1, kOut) == kTag_);
    CHECK(a.call("cellSearchStartSceneSearch", 1, kTitle, 0, 1, 1, 1, kOut) == kTag_);
    ps_write32(base, kTags + 4, 0);
    CHECK(a.call("cellSearchStartSceneSearch", 1, kTitle, kTags, 2, 1, 1, kOut) == kTag_);
    std::memset(base + kText, 'a', 64);
    ps_write32(base, kTags + 4, kText);
    CHECK(a.call("cellSearchStartSceneSearch", 1, kTitle, kTags, 2, 1, 1, kOut) == kTag_); // no NUL in 63 bytes
    base[kText + 62] = 0;
    CHECK(a.call("cellSearchStartSceneSearch", 1, kTitle, kTags, 2, 2, 2, kOut) == kNotSupported);
    CHECK(a.call("cellSearchStartSceneSearch", 1, kTitle, kTags, 2, 5, 1, kOut) == kNotSupported);
    REQUIRE(a.call("cellSearchStartSceneSearch", 5, kTitle, kTags, 2, 2, 1, kOut) == 0);
    CHECK(ps_read32(base, kOut) == 5);
    e = take(a, base, 4);
    REQUIRE(e.size() == 1);
    CHECK(e[0].r3 == 7);
    CHECK(e[0].p0 == 5);

    // cellSearchEnd: the ID, the state, then the search, which goes.
    CHECK(a.call("cellSearchEnd", 0) == kBadId);
    CHECK(a.call("cellSearchEnd", 6) == kBadId);
    REQUIRE(a.call("cellSearchEnd", 1) == 0);
    CHECK(a.call("cellSearchEnd", 1) == kBadId);
    CHECK(a.call("cellSearchGetContentInfoByOffset", 1, 0, kBuf, kType, kId) == kBadId);

    // Finalize: busy until the game's next check; then nothing works until the next initialize. IDs go on.
    REQUIRE(a.call("cellSearchFinalize") == 0);
    CHECK(a.call("cellSearchFinalize") == kFinalizing);
    CHECK(a.call("cellSearchInitialize", 0, 0xFFFFFFFE, kOpd, kUser) == kFinalizing);
    CHECK(a.call("cellSearchStartListSearch", 1, 1, kOut) == kFinalizing);
    CHECK(take(a, base, 4) == std::vector<Entry>{{2, 0, 0, kUser, 0, 0}});
    CHECK(a.call("cellSearchStartListSearch", 1, 1, kOut) == kNotInit);
    REQUIRE(a.call("cellSearchInitialize", 0, 0xFFFFFFFE, kOpd, kUser) == 0);
    CHECK(take(a, base, 4).size() == 1);
    REQUIRE(a.call("cellSearchStartListSearch", 1, 1, kOut) == 0);
    CHECK(ps_read32(base, kOut) == 6);
    CHECK(take(a, base, 4).size() == 1);
}

TEST_CASE("cellMusic: no music is ever selected; volume kept; results through the callback (D-186)",
    "[hle][media][music]")
{
    InterpretMisses interpret;
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* base = f.base();
    write_callback(base, false);
    constexpr u32 kStatus = kData + 0x100, kLevel = kData + 0x110, kId = kData + 0x120, kSpurs = kData + 0x200,
                  kPrio = kData + 0x300, kCtx = kData + 0x1000;
    const u64 kParam = code(0x8002C102), kNoActive = code(0x8002C104), kGeneric = code(0x8002C1FF);
    constexpr u32 kInvalidContext = 0x8002C106, kGenericCode = 0x8002C1FF;
    const auto volume = [&](std::string_view fn) {
        REQUIRE(a.call(fn, kLevel) == 0);
        return std::bit_cast<f32>(ps_read32(base, kLevel));
    };
    const auto set_volume = [&](std::string_view fn, double level) {
        a.ctx.fpr[1] = level;
        return a.call(fn);
    };

    // Before initialize: the callback is needed for anything it would hear; the player's state is readable.
    CHECK(a.call("cellMusicSelectContents", 0) == kGeneric);
    CHECK(set_volume("cellMusicSetVolume", 0.5) == kGeneric);
    CHECK(a.call("cellMusicSetPlaybackCommand", 1, 0) == kGeneric);
    CHECK(a.call("cellMusicSetSelectionContext", kCtx) == kGeneric);
    ps_write32(base, kStatus, 0xAAAAAAAA);
    REQUIRE(a.call("cellMusicGetPlaybackStatus", kStatus) == 0);
    CHECK(ps_read32(base, kStatus) == 0); // STOP
    CHECK(volume("cellMusicGetVolume") == 1.0f);
    CHECK(a.call("cellMusicGetVolume", 0) == kParam);
    CHECK(a.call("cellMusicGetVolume", kUnmapped) == kParam); // RPCS3 faults
    CHECK(a.call("cellMusicGetSelectionContext", 0) == kParam);
    CHECK(a.call("cellMusicGetSelectionContext", kCtx) == kNoActive);
    CHECK(a.call("cellMusicGetContentsId2", kId) == kNoActive);
    CHECK(a.call("cellMusicGetContentsId", 0) == kParam);

    // The four initialize forms: their checks; the callback hears INITIALIZE_RESULT with the user data given.
    CHECK(a.call("cellMusicInitialize", 1, 0, 100, kOpd, 0x55) == kParam);
    CHECK(a.call("cellMusicInitialize", 0, 0, 15, kOpd, 0x55) == kParam);
    CHECK(a.call("cellMusicInitialize", 0, 0, 256, kOpd, 0x55) == kParam);
    CHECK(a.call("cellMusicInitialize", 0, 0, 100, 0, 0x55) == kParam);
    REQUIRE(a.call("cellMusicInitialize", 0, 0, 100, kOpd, 0x55) == 0);
    CHECK(take(a, base, 3) == std::vector<Entry>{{1, 0, 0x55, 0, 0, 0}});
    CHECK(a.call("cellMusicInitialize2", 0, 255, 0, 0x56) == kParam);
    REQUIRE(a.call("cellMusicInitialize2", 0, 16, kOpd, 0x56) == 0);
    CHECK(take(a, base, 3) == std::vector<Entry>{{1, 0, 0x56, 0, 0, 0}});
    CHECK(a.call("cellMusicInitializeSystemWorkload", 0, 0, kOpd, 0x57, 0, kPrio, 0) == kParam);
    CHECK(a.call("cellMusicInitializeSystemWorkload", 0, 0, kOpd, 0x57, kSpurs, 0, 0) == kParam);
    REQUIRE(a.call("cellMusicInitializeSystemWorkload", 0, 0, kOpd, 0x57, kSpurs, kPrio, 0) == 0);
    CHECK(take(a, base, 3).size() == 1);
    CHECK(a.call("cellMusicInitialize2SystemWorkload", 1, kOpd, 0x58, kSpurs, kPrio, 0) == kParam);
    REQUIRE(a.call("cellMusicInitialize2SystemWorkload", 0, kOpd, 0x58, kSpurs, kPrio, 0) == 0);
    CHECK(take(a, base, 3) == std::vector<Entry>{{1, 0, 0x58, 0, 0, 0}});

    // Selecting music: no media dialog, so cancelled (CELL_MUSIC_CANCELED, 1) at the next check.
    REQUIRE(a.call("cellMusicSelectContents", 0xFFFFFFFE) == 0);
    REQUIRE(a.call("cellMusicSelectContents2") == 0);
    CHECK(take(a, base, 3) == std::vector<Entry>{{3, 1, 0x58, 0, 0, 0}, {3, 1, 0x58, 0, 0, 0}});
    CHECK(a.call("cellMusicGetSelectionContext2", kCtx) == kNoActive);

    // A saved selection context names no playlist pspocket has: INVALID_CONTEXT to the callback.
    std::memset(base + kCtx, 0, 2048);
    put(base, kCtx + 4, "music_selection_context_0");
    std::memcpy(base + kCtx, "SUS", 4);
    CHECK(a.call("cellMusicSetSelectionContext2", 0) == kParam);
    CHECK(a.call("cellMusicSetSelectionContext2", kUnmapped) == kParam); // RPCS3 faults
    REQUIRE(a.call("cellMusicSetSelectionContext", kCtx) == 0);
    REQUIRE(a.call("cellMusicSetSelectionContext2", kCtx) == 0);
    CHECK(take(a, base, 3) ==
          std::vector<Entry>{{6, kInvalidContext, 0x58, 0, 0, 0}, {6, kInvalidContext, 0x58, 0, 0, 0}});

    // Playback: STOP to FASTREVERSE; nothing selected, so ERROR_GENERIC, returned and to the callback.
    CHECK(a.call("cellMusicSetPlaybackCommand", 7, 0) == kParam);
    CHECK(a.call("cellMusicSetPlaybackCommand2", -1, 0) == kParam);
    CHECK(a.call("cellMusicSetPlaybackCommand", 1, 0) == kGeneric);
    CHECK(a.call("cellMusicSetPlaybackCommand2", 0, 0) == kGeneric);
    CHECK(take(a, base, 3) ==
          std::vector<Entry>{{4, kGenericCode, 0x58, 0, 0, 0}, {4, kGenericCode, 0x58, 0, 0, 0}});
    REQUIRE(a.call("cellMusicGetPlaybackStatus2", kStatus) == 0);
    CHECK(ps_read32(base, kStatus) == 0);

    // Volume: clamped to 0..1 and kept (cellMusic and cellMusic2 share it); SET_VOLUME_RESULT to the callback.
    REQUIRE(set_volume("cellMusicSetVolume", 2.0) == 0);
    CHECK(volume("cellMusicGetVolume2") == 1.0f);
    REQUIRE(set_volume("cellMusicSetVolume2", 0.25) == 0);
    CHECK(volume("cellMusicGetVolume") == 0.25f);
    REQUIRE(set_volume("cellMusicSetVolume", -1.0) == 0);
    CHECK(volume("cellMusicGetVolume") == 0.0f);
    CHECK(take(a, base, 3).size() == 3);

    // Finalize: FINALIZE_RESULT; the callback stays (as in RPCS3).
    REQUIRE(a.call("cellMusicFinalize") == 0);
    REQUIRE(a.call("cellMusicFinalize2") == 0);
    CHECK(take(a, base, 3) == std::vector<Entry>{{2, 0, 0x58, 0, 0, 0}, {2, 0, 0x58, 0, 0, 0}});
    REQUIRE(a.call("cellMusicSelectContents2") == 0);
    CHECK(take(a, base, 3).size() == 1);
}

TEST_CASE("cellMusicDecode: commands, status and reads with nothing selected; two independent decoders (D-186)",
    "[hle][media][music]")
{
    InterpretMisses interpret;
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* base = f.base();
    write_callback(base, false);
    constexpr u32 kStatus = kData + 0x100, kStart = kData + 0x110, kRead = kData + 0x118, kPos = kData + 0x120,
                  kId = kData + 0x130, kSpurs = kData + 0x200, kPrio = kData + 0x300, kCtx = kData + 0x1000,
                  kBuf = kData + 0x2000;
    const u64 kParam = code(0x8002C102), kFailure = code(0x8002C107), kNoData = code(0x8002C10B),
              kGeneric = code(0x8002C1FF), kNotFound = code(0x8002C80A);
    constexpr u32 kInvalidContext = 0x8002C106, kNoMore = 0x8002C108;
    const auto status = [&](std::string_view fn) {
        REQUIRE(a.call(fn, kStatus) == 0);
        return ps_read32(base, kStatus);
    };
    const auto read = [&](std::string_view fn) {
        ps_write32(base, kStart, 0xAAAAAAAA);
        ps_write64(base, kRead, ~u64{0});
        ps_write32(base, kPos, 0xAAAAAAAA);
        const u64 r = a.call(fn, kBuf, kStart, 0x1000, kRead, kPos);
        CHECK(ps_read32(base, kStart) == 0);
        CHECK(ps_read64(base, kRead) == 0);
        CHECK(ps_read32(base, kPos) == 0); // POSITION_NONE
        return r;
    };

    // Before initialize.
    CHECK(a.call("cellMusicDecodeSetDecodeCommand", 0) == kGeneric);
    CHECK(a.call("cellMusicDecodeSelectContents") == kGeneric);
    CHECK(a.call("cellMusicDecodeSetSelectionContext", kCtx) == kGeneric);
    CHECK(status("cellMusicDecodeGetDecodeStatus") == 0); // DORMANT
    CHECK(read("cellMusicDecodeRead") == kNoData);
    CHECK(a.call("cellMusicDecodeRead", kBuf, kStart, 0, kRead, kPos) == kParam);
    CHECK(a.call("cellMusicDecodeRead", 0, kStart, 0x1000, kRead, kPos) == kParam);
    std::memset(base + kCtx, 0xAA, 2048);
    REQUIRE(a.call("cellMusicDecodeGetSelectionContext", kCtx) == 0);
    CHECK(std::memcmp(base + kCtx, "SUS", 4) == 0);
    CHECK(all_bytes(base + kCtx + 4, 2044, 0));
    CHECK(a.call("cellMusicDecodeGetContentsId", kId) == kNotFound); // the library is empty
    CHECK(a.call("cellMusicDecodeGetContentsId2", 0) == kParam);

    // Initialize: the mode, an SPU priority of 16 to 255 or a usage rate of 1 to 100, the callback.
    CHECK(a.call("cellMusicDecodeInitialize", 1, 0, 100, kOpd, 0x61) == kParam);
    CHECK(a.call("cellMusicDecodeInitialize", 0, 0, 15, kOpd, 0x61) == kParam);
    CHECK(a.call("cellMusicDecodeInitialize", 0, 0, 100, 0, 0x61) == kParam);
    CHECK(a.call("cellMusicDecodeInitializeSystemWorkload", 0, 0, kOpd, 0x61, 0, kSpurs, kPrio, 0) == kParam);
    CHECK(a.call("cellMusicDecodeInitializeSystemWorkload", 0, 0, kOpd, 0x61, 101, kSpurs, kPrio, 0) == kParam);
    REQUIRE(a.call("cellMusicDecodeInitializeSystemWorkload", 0, 0, kOpd, 0x60, 100, kSpurs, kPrio, 0) == 0);
    REQUIRE(a.call("cellMusicDecodeInitialize", 0, 0, 100, kOpd, 0x61) == 0);
    CHECK(take(a, base, 3) == std::vector<Entry>{{1, 0, 0x60, 0, 0, 0}, {1, 0, 0x61, 0, 0, 0}});

    // Commands: START decodes an empty playlist, which fails; NEXT finds no next track; STOP ends it.
    CHECK(a.call("cellMusicDecodeSetDecodeCommand", 4) == kParam);
    CHECK(a.call("cellMusicDecodeSetDecodeCommand", -1) == kParam);
    REQUIRE(a.call("cellMusicDecodeSetDecodeCommand", 1) == 0);
    CHECK(status("cellMusicDecodeGetDecodeStatus") == 1); // DECODING
    CHECK(read("cellMusicDecodeRead") == kFailure);
    REQUIRE(a.call("cellMusicDecodeSetDecodeCommand", 2) == 0);
    CHECK(status("cellMusicDecodeGetDecodeStatus") == 0);
    CHECK(read("cellMusicDecodeRead") == kNoData);
    REQUIRE(a.call("cellMusicDecodeSetDecodeCommand", 1) == 0);
    REQUIRE(a.call("cellMusicDecodeSetDecodeCommand", 0) == 0);
    CHECK(status("cellMusicDecodeGetDecodeStatus") == 0);
    CHECK(read("cellMusicDecodeRead") == kNoData);
    REQUIRE(a.call("cellMusicDecodeSetDecodeCommand", 3) == 0);
    CHECK(take(a, base, 3) == std::vector<Entry>{{4, 0, 0x61, 0, 0, 0}, {4, kNoMore, 0x61, 0, 0, 0},
                               {4, 0, 0x61, 0, 0, 0}, {4, 0, 0x61, 0, 0, 0}, {4, kNoMore, 0x61, 0, 0, 0}});

    // A selection context: INVALID_CONTEXT to the callback; a well-formed hash is kept and read back.
    std::memset(base + kCtx, 0, 2048);
    std::memcpy(base + kCtx, "SUS", 4);
    put(base, kCtx + 4, "abc-1_X");
    REQUIRE(a.call("cellMusicDecodeSetSelectionContext", kCtx) == 0);
    put(base, kCtx + 4, "bad hash!");
    REQUIRE(a.call("cellMusicDecodeSetSelectionContext", kCtx) == 0);
    std::memcpy(base + kCtx, "XYZ", 4);
    REQUIRE(a.call("cellMusicDecodeSetSelectionContext", kCtx) == 0);
    CHECK(take(a, base, 3) == std::vector<Entry>{{5, kInvalidContext, 0x61, 0, 0, 0}, {5, kInvalidContext, 0x61, 0, 0, 0},
                               {5, kInvalidContext, 0x61, 0, 0, 0}});
    REQUIRE(a.call("cellMusicDecodeGetSelectionContext", kCtx) == 0);
    CHECK(std::memcmp(base + kCtx, "SUS\0abc-1_X\0", 12) == 0);

    // Selecting music: cancelled. Finalize: dormant, FINALIZE_RESULT.
    REQUIRE(a.call("cellMusicDecodeSelectContents") == 0);
    REQUIRE(a.call("cellMusicDecodeSetDecodeCommand", 1) == 0);
    REQUIRE(a.call("cellMusicDecodeFinalize") == 0);
    CHECK(status("cellMusicDecodeGetDecodeStatus") == 0);
    CHECK(take(a, base, 3) == std::vector<Entry>{{3, 1, 0x61, 0, 0, 0}, {4, 0, 0x61, 0, 0, 0}, {2, 0, 0x61, 0, 0, 0}});

    // cellMusicDecode2 has its own state: a buffer of at least 448 KB, speed MAX or 2.
    CHECK(a.call("cellMusicDecodeSetDecodeCommand2", 1) == kGeneric);
    CHECK(a.call("cellMusicDecodeInitialize2", 0, 0, 100, kOpd, 0x62, 1, 448 * 1024) == kParam);
    CHECK(a.call("cellMusicDecodeInitialize2", 0, 0, 100, kOpd, 0x62, 2, 448 * 1024 - 1) == kParam);
    CHECK(a.call("cellMusicDecodeInitialize2SystemWorkload", 0, 0, kOpd, 0x62, 50, 448 * 1024, 0, kPrio, 0) ==
          kParam);
    REQUIRE(a.call("cellMusicDecodeInitialize2SystemWorkload", 0, 0, kOpd, 0x62, 50, 448 * 1024, kSpurs, kPrio, 0) ==
            0);
    REQUIRE(a.call("cellMusicDecodeInitialize2", 0, 0, 100, kOpd, 0x63, 2, 448 * 1024) == 0);
    REQUIRE(a.call("cellMusicDecodeSetDecodeCommand2", 1) == 0);
    CHECK(status("cellMusicDecodeGetDecodeStatus2") == 1);
    CHECK(status("cellMusicDecodeGetDecodeStatus") == 0);
    CHECK(read("cellMusicDecodeRead2") == kFailure);
    CHECK(read("cellMusicDecodeRead") == kNoData);
    REQUIRE(a.call("cellMusicDecodeGetSelectionContext2", kCtx) == 0);
    CHECK(all_bytes(base + kCtx + 4, 2044, 0)); // not the first decoder's hash
    REQUIRE(a.call("cellMusicDecodeSelectContents2") == 0);
    REQUIRE(a.call("cellMusicDecodeFinalize2") == 0);
    CHECK(status("cellMusicDecodeGetDecodeStatus2") == 0);
    CHECK(take(a, base, 3) == std::vector<Entry>{{1, 0, 0x62, 0, 0, 0}, {1, 0, 0x63, 0, 0, 0}, {4, 0, 0x63, 0, 0, 0},
                               {3, 1, 0x63, 0, 0, 0}, {2, 0, 0x63, 0, 0, 0}});
}

TEST_CASE("Media exports: files moved or copied into /dev_hdd0's music, video and photo folders (D-186)",
    "[hle][media][export]")
{
    InterpretMisses interpret;
    TempHdd hdd("export");
    hle::ProcessConfig config;
    config.game.title_id = "NPEB00001";
    Fixture f(std::move(config));
    REQUIRE(f.kernel->vfs_mount("/dev_hdd0", hdd.root.string(), false));
    GuestThread a(f, 0, "a");
    u8* base = f.base();
    write_callback(base, false);
    constexpr u32 kDir = kData + 0x100, kFile = kData + 0x200, kParam = kData + 0x300, kPath = kData + 0x400;
    constexpr u32 kUser = 0x99;
    const u64 kMusicParam = code(0x8002C603), kPhotoParam = code(0x8002C203), kVideoParam = code(0x8002CA03);
    constexpr u32 kMusicMove = 0x8002C609;
    std::memset(base + kParam, 0, 32);

    // Initialize, Finalize: the version and the callback, which hears CELL_OK.
    CHECK(a.call("cellMusicExportInitialize", 1, 0xFFFFFFFE, kOpd, kUser) == kMusicParam);
    CHECK(a.call("cellMusicExportInitialize", 0, 0xFFFFFFFE, 0, kUser) == kMusicParam);
    REQUIRE(a.call("cellMusicExportInitialize", 0, 0xFFFFFFFE, kOpd, kUser) == 0);
    REQUIRE(a.call("cellMusicExportInitialize2", 0, kOpd, kUser) == 0);
    CHECK(a.call("cellMusicExportFinalize", 0, kUser) == kMusicParam);
    REQUIRE(a.call("cellMusicExportFinalize", kOpd, kUser) == 0);
    REQUIRE(a.call("cellPhotoInitialize", 0, 0xFFFFFFFE, kOpd, kUser) == 0);
    REQUIRE(a.call("cellPhotoExportInitialize2", 0, kOpd, kUser) == 0);
    REQUIRE(a.call("cellVideoExportInitialize", 0, 0xFFFFFFFE, kOpd, kUser) == 0);
    CHECK(a.call("cellVideoExportInitialize2", 2, kOpd, kUser) == kVideoParam);
    CHECK(take(a, base, 2) == std::vector<Entry>(6, Entry{0, kUser, 0, 0, 0, 0}));

    // A music export: moved to /dev_hdd0/music, "_0" added when the name is taken; the progress follows.
    hdd.write("tmp/song.mp3", "first");
    put(base, kDir, "/dev_hdd0/tmp");
    put(base, kFile, "song.mp3");
    CHECK(a.call("cellMusicExportFromFile", kDir, kFile, 0, kOpd, kUser) == kMusicParam);
    CHECK(a.call("cellMusicExportFromFile", kDir, kFile, kParam, 0, kUser) == kMusicParam);
    REQUIRE(a.call("cellMusicExportFromFile", kDir, kFile, kParam, kOpd, kUser) == 0);
    REQUIRE(a.call("cellMusicExportProgress", kOpd, kUser) == 0);
    CHECK(take(a, base, 2) == std::vector<Entry>{{0, kUser, 0, 0, 0, 0}, {0xFFFF, kUser, 0, 0, 0, 0}});
    CHECK(hdd.read("music/song.mp3") == "first");
    CHECK_FALSE(fsys::exists(hdd.root / "tmp" / "song.mp3"));
    hdd.write("tmp/song.mp3", "second");
    REQUIRE(a.call("cellMusicExportFromFile", kDir, kFile, kParam, kOpd, kUser) == 0);
    CHECK(take(a, base, 2).size() == 1);
    CHECK(hdd.read("music/song_0.mp3") == "second");
    // A missing file: the copy fails (ERROR_MOVE) and the progress stays at 0.
    REQUIRE(a.call("cellMusicExportFromFile", kDir, kFile, kParam, kOpd, kUser) == 0);
    REQUIRE(a.call("cellMusicExportProgress", kOpd, kUser) == 0);
    CHECK(take(a, base, 2) == std::vector<Entry>{{kMusicMove, kUser, 0, 0, 0, 0}, {0, kUser, 0, 0, 0, 0}});
    // RPCS3's path rule: letters, digits, "-_/." only, no "..", on /dev_hdd0, /dev_hdd1 or /dev_bdvd; a file name.
    for (const std::string_view dir : {"/dev_hdd0/my dir", "/dev_hdd0/../x", "/dev_usb000/x", "dev_hdd0/x"}) {
        INFO(dir);
        put(base, kDir, dir);
        CHECK(a.call("cellMusicExportFromFile", kDir, kFile, kParam, kOpd, kUser) == kMusicParam);
    }
    put(base, kDir, "/dev_hdd0/tmp");
    put(base, kFile, "");
    CHECK(a.call("cellMusicExportFromFile", kDir, kFile, kParam, kOpd, kUser) == kMusicParam);
    CHECK(a.call("cellMusicExportFromFile", 0, kFile, kParam, kOpd, kUser) == kMusicParam); // RPCS3 faults
    CHECK(take(a, base, 2).empty());

    // Videos: the strings must not be empty; FromFileWithCopy keeps the file, FromFile moves it.
    hdd.write("tmp/clip.mp4", "video");
    put(base, kFile, "clip.mp4");
    put(base, kPath, "");
    CHECK(a.call("cellVideoExportFromFile", kPath, kFile, kParam, kOpd, kUser) == kVideoParam);
    CHECK(a.call("cellVideoExportFromFile", 0, kFile, kParam, kOpd, kUser) == kVideoParam);
    REQUIRE(a.call("cellVideoExportFromFileWithCopy", kDir, kFile, kParam, kOpd, kUser) == 0);
    REQUIRE(a.call("cellVideoExportFromFile", kDir, kFile, kParam, kOpd, kUser) == 0);
    REQUIRE(a.call("cellVideoExportProgress", kOpd, kUser) == 0);
    REQUIRE(a.call("cellVideoExportFinalize", kOpd, kUser) == 0);
    CHECK(take(a, base, 2) == std::vector<Entry>{{0, kUser, 0, 0, 0, 0}, {0, kUser, 0, 0, 0, 0},
                               {0xFFFF, kUser, 0, 0, 0, 0}, {0, kUser, 0, 0, 0, 0}});
    CHECK(hdd.read("video/clip.mp4") == "video");
    CHECK(hdd.read("video/clip_0.mp4") == "video");
    CHECK_FALSE(fsys::exists(hdd.root / "tmp" / "clip.mp4"));

    // Photos: /dev_hdd0/photo/YYYY/MM/DD/<the game's title> DD-MM-YYYY HH-MM-SS<ext>; ".png" for a name without one.
    hdd.write("tmp/shot.jpg", "jpeg");
    hdd.write("tmp/pic", "png");
    put(base, kPath, "/dev_hdd0/tmp/shot.jpg");
    CHECK(a.call("cellPhotoRegistFromFile", 0, 0, 0, 0, kOpd, kUser) == kPhotoParam);
    REQUIRE(a.call("cellPhotoRegistFromFile", kPath, 0, 0, 0, kOpd, kUser) == 0);
    put(base, kFile, "pic");
    REQUIRE(a.call("cellPhotoExportFromFileWithCopy", kDir, kFile, kParam, kOpd, kUser) == 0);
    REQUIRE(a.call("cellPhotoExportProgress", kOpd, kUser) == 0);
    REQUIRE(a.call("cellPhotoExportFinalize", kOpd, kUser) == 0);
    REQUIRE(a.call("cellPhotoFinalize", kOpd, kUser) == 0);
    CHECK(take(a, base, 2) == std::vector<Entry>{{0, kUser, 0, 0, 0, 0}, {0, kUser, 0, 0, 0, 0},
                               {0xFFFF, kUser, 0, 0, 0, 0}, {0, kUser, 0, 0, 0, 0}, {0, kUser, 0, 0, 0, 0}});
    std::vector<std::string> photos;
    for (const auto& entry : fsys::recursive_directory_iterator(hdd.root / "photo")) {
        if (entry.is_regular_file()) photos.push_back(entry.path().filename().string());
    }
    std::sort(photos.begin(), photos.end());
    REQUIRE(photos.size() == 2);
    CHECK(photos[0].starts_with("NPEB00001 "));
    CHECK(photos[0].ends_with(".jpg"));
    CHECK(photos[1].starts_with("NPEB00001 "));
    CHECK(photos[1].ends_with(".png"));
    CHECK_FALSE(fsys::exists(hdd.root / "tmp" / "shot.jpg")); // moved from /dev_hdd0
    CHECK(fsys::exists(hdd.root / "tmp" / "pic"));             // WithCopy keeps it
}

TEST_CASE("cellPhotoImport is cancelled at once; cellPhotoDecode checks its file, then cannot decode (D-186)",
    "[hle][media][photo]")
{
    InterpretMisses interpret;
    TempHdd hdd("import");
    Fixture f;
    REQUIRE(f.kernel->vfs_mount("/dev_hdd0", hdd.root.string(), false));
    GuestThread a(f, 0, "a");
    u8* base = f.base();
    write_callback(base, false);
    constexpr u32 kDst = kData + 0x100, kSet = kData + 0x200, kRet = kData + 0x300, kDir = kData + 0x400,
                  kFile = kData + 0x500;
    constexpr u32 kUser = 0x42;
    const u64 kImportBusy = code(0x8002C701), kImportParam = code(0x8002C703), kImportAccess = code(0x8002C704);
    const u64 kDecodeParam = code(0x8002C903), kDecodeAccess = code(0x8002C904), kDecodeError = code(0x8002C906);
    fsys::create_directories(hdd.root / "game" / "TEST00001" / "USRDIR");
    ps_write32(base, kSet, 0x100000); // fileSizeMax

    // Import: the version, callback, parameters and destination; busy until the cancelled answer.
    put(base, kDst, "/dev_hdd0/game/TEST00001/USRDIR");
    CHECK(a.call("cellPhotoImport", 1, kDst, kSet, 0xFFFFFFFF, kOpd, kUser) == kImportParam);
    CHECK(a.call("cellPhotoImport", 0, kDst, 0, 0xFFFFFFFF, kOpd, kUser) == kImportParam);
    CHECK(a.call("cellPhotoImport2", 0, 0, kSet, kOpd, kUser) == kImportParam);
    CHECK(a.call("cellPhotoImport2", 0, kDst, kSet, 0, kUser) == kImportParam);
    REQUIRE(a.call("cellPhotoImport", 0, kDst, kSet, 0xFFFFFFFF, kOpd, kUser) == 0);
    CHECK(a.call("cellPhotoImport2", 0, kDst, kSet, kOpd, kUser) == kImportBusy);
    std::vector<Entry> e = take(a, base, 3);
    REQUIRE(e.size() == 1);
    CHECK(e[0].r3 == 1); // CELL_CANCEL
    CHECK(e[0].r4 != 0);
    CHECK(e[0].r5 == kUser);
    // The file data, cleared: cellPhotoImportUtil's variables under RPCS3's names (D-567).
    const u32 file_data = f.hle->link_variable("cellPhotoImportUtil", recomp::nid_of("g_filedata"));
    const u32 file_data_sub = f.hle->link_variable("cellPhotoImportUtil", recomp::nid_of("g_filedata_sub"));
    CHECK(e[0].r4 == file_data);
    CHECK(all_bytes(base + file_data, 1608, 0));
    CHECK(all_bytes(base + file_data_sub, 16, 0));
    put(base, kDst, "/dev_usb000/photos");
    CHECK(a.call("cellPhotoImport2", 0, kDst, kSet, kOpd, kUser) == kImportAccess);
    put(base, kDst, "/dev_hdd0/nothere");
    CHECK(a.call("cellPhotoImport2", 0, kDst, kSet, kOpd, kUser) == kImportAccess);
    put(base, kDst, "/dev_hdd0/game/TEST00001/USRDIR");
    REQUIRE(a.call("cellPhotoImport2", 0, kDst, kSet, kOpd, kUser) == 0);
    CHECK(take(a, base, 3).size() == 1);

    // Decode: initialize and finalize through the callback; a file is checked, then there is no decoder.
    CHECK(a.call("cellPhotoDecodeInitialize", 1, 0xFFFFFFFF, 0xFFFFFFFF, kOpd, kUser) == kDecodeParam);
    REQUIRE(a.call("cellPhotoDecodeInitialize", 0, 0xFFFFFFFF, 0xFFFFFFFF, kOpd, kUser) == 0);
    REQUIRE(a.call("cellPhotoDecodeInitialize2", 0, 0xFFFFFFFF, kOpd, kUser) == 0);
    CHECK(a.call("cellPhotoDecodeFinalize", 0, kUser) == kDecodeParam);
    REQUIRE(a.call("cellPhotoDecodeFinalize", kOpd, kUser) == 0);
    CHECK(take(a, base, 2) == std::vector<Entry>(3, Entry{0, kUser, 0, 0, 0, 0}));
    hdd.write("tmp/img.png", "not really a png");
    put(base, kDir, "/dev_hdd0/tmp");
    put(base, kFile, "img.png");
    ps_write32(base, kSet, kData + 0x10000); // dstBuffer
    ps_write16(base, kSet + 4, 64);
    ps_write16(base, kSet + 6, 64);
    CHECK(a.call("cellPhotoDecodeFromFile", 0, kFile, kSet, kRet) == kDecodeParam);
    CHECK(a.call("cellPhotoDecodeFromFile", kDir, kFile, kSet, 0) == kDecodeParam);
    std::memset(base + kRet, 0xAA, 12);
    put(base, kDir, "/dev_usb000");
    CHECK(a.call("cellPhotoDecodeFromFile", kDir, kFile, kSet, kRet) == kDecodeAccess);
    CHECK(all_bytes(base + kRet, 12, 0));
    put(base, kDir, "/dev_hdd0/tmp");
    put(base, kFile, "missing.png");
    CHECK(a.call("cellPhotoDecodeFromFile", kDir, kFile, kSet, kRet) == kDecodeAccess);
    put(base, kFile, "img.png");
    CHECK(a.call("cellPhotoDecodeFromFile", kDir, kFile, kSet, kRet) == kDecodeError);
}
