// cellRec, cellSubDisplay and libfs_utility_init's partition count (synthetic: no game data): RPCS3's checks and
// answers; a recording cannot start (no video encoder), the sub display has no device registered (D-189).

#include <catch_amalgamated.hpp>

#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hle_test_util.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::ppu::as;

namespace {

struct InterpretMisses {
    const ps_ppu_miss_policy saved = ps_ppu_get_miss_policy();
    InterpretMisses() { ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET); }
    ~InterpretMisses() { ps_ppu_set_miss_policy(saved); }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

// A guest callback that logs the low words of r3..r5 (kLog: the count, then 16 bytes an entry).
constexpr u32 kCode = kData + 0x20000, kOpd = kData + 0x20800, kLog = kData + 0x21000;

void write_callback(u8* base)
{
    const std::vector<u32> code = {
        as::lis(7, static_cast<s32>(kLog >> 16)), as::ori(7, 7, kLog & 0xFFFF), as::lwz(8, 0, 7), as::slwi(9, 8, 4),
        as::addi(9, 9, 16), as::stwx(3, 7, 9), as::addi(10, 9, 4), as::stwx(4, 7, 10), as::addi(10, 9, 8),
        as::stwx(5, 7, 10), as::addi(8, 8, 1), as::stw(8, 0, 7), as::blr(),
    };
    for (usize i = 0; i < code.size(); i++) ps_write32(base, kCode + 4 * static_cast<u32>(i), code[i]);
    ps_write32(base, kOpd, kCode);
    ps_write32(base, kOpd + 4, 0x1234);
    std::memset(base + kLog, 0, 0x400);
}

struct Entry {
    u32 status, error, user;
    bool operator==(const Entry&) const = default;
};

std::vector<Entry> take(GuestThread& g, u8* base)
{
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    std::vector<Entry> v;
    for (u32 i = 0; i < ps_read32(base, kLog); i++) {
        const u32 at = kLog + 16 + 16 * i;
        v.push_back({ps_read32(base, at), ps_read32(base, at + 4), ps_read32(base, at + 8)});
    }
    std::memset(base + kLog, 0, 0x400);
    return v;
}

void put(u8* base, u32 addr, std::string_view s)
{
    std::memcpy(base + addr, s.data(), s.size());
    base[addr + s.size()] = 0;
}

// A CellRecParam (16 bytes) with `options` CellRecOption entries (16 bytes each: option, value at 8) at `opts`.
void put_param(u8* b, u32 at, s32 video, s32 audio, u32 opts = 0, std::vector<std::pair<s32, u32>> options = {})
{
    ps_write32(b, at, static_cast<u32>(video));
    ps_write32(b, at + 4, static_cast<u32>(audio));
    ps_write32(b, at + 8, static_cast<u32>(options.size()));
    ps_write32(b, at + 12, opts);
    for (u32 i = 0; i < options.size(); i++) {
        std::memset(b + opts + 16 * i, 0, 16);
        ps_write32(b, opts + 16 * i, static_cast<u32>(options[i].first));
        ps_write32(b, opts + 16 * i + 8, options[i].second);
    }
}

u64 rec_code(u32 c) { return r3_of_code(c); }

} // namespace

TEST_CASE("cellRec: RPCS3's open checks, input buffers and info; a recording cannot start without an encoder",
    "[hle][rec]")
{
    InterpretMisses interpret;
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* base = f.base();
    write_callback(base);
    constexpr u32 kDir = kData + 0x100, kFile = kData + 0x200, kParam = kData + 0x300, kOpts = kData + 0x400,
                  kSpurs = kData + 0x500, kValue = kData + 0x600, kMeta = kData + 0x700, kText = kData + 0x800,
                  kText2 = kData + 0x900, kTags = kData + 0xA00, kUser = 0x66;
    const u64 kInval = rec_code(0x8002C503), kState = rec_code(0x8002C506);
    constexpr u32 kFileOpen = 0x8002C504, kNoData = 0x8002C507;
    put(base, kDir, "/dev_hdd0/game/TEST00001/USRDIR");
    put(base, kFile, "movie.mp4");
    const auto open = [&](u32 container = 1, u32 cb = kOpd) {
        return a.call("cellRecOpen", kDir, kFile, kParam, container, cb, kUser);
    };
    const auto info = [&](s32 which) -> u64 {
        ps_write64(base, kValue, 0xAAAAAAAAAAAAAAAAull);
        REQUIRE(a.call("cellRecGetInfo", which, kValue) == 0);
        return ps_read64(base, kValue);
    };

    // The memory a recording needs, by format and options.
    CHECK(a.call("cellRecQueryMemSize", 0) == 0x900000);
    put_param(base, kParam, 0x0100, 0); // MPEG-4 middle, AAC
    CHECK(a.call("cellRecQueryMemSize", kParam) == 0x800000);
    put_param(base, kParam, 0x1000, 0);
    CHECK(a.call("cellRecQueryMemSize", kParam) == 0x900000);
    put_param(base, kParam, 0x4640, 0); // M4HD HD720
    CHECK(a.call("cellRecQueryMemSize", kParam) == 0x1100000);
    put_param(base, kParam, 0x3060, 0x2008, kOpts, {{12, 1}}); // MJPEG small, reduced
    CHECK(a.call("cellRecQueryMemSize", kParam) == 0);
    put_param(base, kParam, 0x3060, 0x2008, kOpts, {{10, 1}}); // MJPEG small, external audio
    CHECK(a.call("cellRecQueryMemSize", kParam) == 0x400000);

    // cellRecOpen's checks: the arguments, the container for the memory, the formats and their pairings, the options,
    // the path, the callback.
    put_param(base, kParam, 0x0100, 0);
    CHECK(a.call("cellRecOpen", kDir, kFile, 0, 1, kOpd, kUser) == kInval);
    CHECK(a.call("cellRecOpen", 0, kFile, kParam, 1, kOpd, kUser) == kInval);
    CHECK(open(0xFFFFFFFF) == kInval); // memory needed but no container
    put_param(base, kParam, 0x5000, 0);
    CHECK(open() == kInval);
    put_param(base, kParam, 0x3060, 0); // AAC with MJPEG
    CHECK(open() == kInval);
    put_param(base, kParam, 0x0100, 0x2007); // PCM with MPEG-4
    CHECK(open() == kInval);
    put_param(base, kParam, 0x1000, 0x1007); // u-law with AVC main profile
    CHECK(open() == kInval);
    put_param(base, kParam, 0x0100, 7);
    CHECK(open() == kInval);
    put_param(base, kParam, 0x0100, 0, kOpts, {{1, 0xC00}});
    CHECK(open() == kInval); // PPU priority
    put_param(base, kParam, 0x0100, 0, kOpts, {{2, 15}});
    CHECK(open() == kInval); // SPU priority 16 to 255
    put_param(base, kParam, 0x0100, 0, kOpts, {{9, 6}});
    CHECK(open() == kInval); // video input
    put_param(base, kParam, 0x3060, 0x2008, kOpts, {{9, 1}});
    CHECK(open() == kInval); // MJPEG takes YUV input only
    put_param(base, kParam, 0x0100, 0, kOpts, {{9, 5}});
    CHECK(open() == kInval); // MPEG-4 takes no YUV input
    put_param(base, kParam, 0x0100, 0, kOpts, {{15, 0}});
    CHECK(open() == kInval); // SPURS parameters
    ps_write32(base, kSpurs, kData + 0x50000);
    ps_write32(base, kSpurs + 4, 0);
    put_param(base, kParam, 0x0100, 0, kOpts, {{15, kSpurs}});
    CHECK(open() == kInval); // usage rate 1 to 100
    put_param(base, kParam, 0x0100, 0);
    put(base, kDir, "/dev_usb000/videos");
    CHECK(open() == kInval);
    put(base, kDir, "/dev_hdd0/game/TEST00001/USRDIR");
    CHECK(open(1, 0) == kInval);
    CHECK(take(a, base).empty());

    // Open: OPEN at the next check; then not again until closed.
    REQUIRE(open() == 0);
    CHECK(take(a, base) == std::vector<Entry>{{1, 0, kUser}});
    CHECK(open() == kState);
    CHECK(info(0) != 0); // the video input buffer
    CHECK(info(4) != 0); // the audio input buffer
    CHECK(info(4) != info(0));
    CHECK(info(1) == 1280); // no external video input: 1280x720
    CHECK(info(2) == 5120);
    CHECK(info(3) == 720);
    CHECK(info(5) == 0); // nothing recorded
    CHECK(info(6) == 0xFFFFFFFFu);
    CHECK(info(99) == 0);
    CHECK(a.call("cellRecGetInfo", 0, 0) == 0); // nothing to write

    // Start: no encoder, so ERR with FILE_OPEN; never recording, so no stop.
    REQUIRE(a.call("cellRecStart") == 0);
    CHECK(take(a, base) == std::vector<Entry>{{10, kFileOpen, kUser}});
    CHECK(a.call("cellRecStop") == kState);
    REQUIRE(a.call("cellRecStart") == 0); // still stopped
    CHECK(take(a, base).size() == 1);

    // Metadata while stopped: the movie's titles, a scene's type, title and tags.
    CHECK(a.call("cellRecSetInfo", 100, 0) == 0);
    CHECK(a.call("cellRecSetInfo", 200, 0) == kInval);
    std::memset(base + kMeta, 0, 40);
    CHECK(a.call("cellRecSetInfo", 200, kMeta) == kInval); // no game title
    put(base, kText, "A Game");
    put(base, kText2, std::string(128, 'm')); // no NUL within 128 bytes
    ps_write32(base, kMeta, kText);
    ps_write32(base, kMeta + 4, kText2);
    CHECK(a.call("cellRecSetInfo", 200, kMeta) == kInval);
    put(base, kText2, "Best lap");
    CHECK(a.call("cellRecSetInfo", 200, kMeta) == 0);
    std::memset(base + kMeta, 0, 40);
    ps_write32(base, kMeta, 3); // type: past CLIP_USER
    ps_write32(base, kMeta + 28, kText);
    CHECK(a.call("cellRecSetInfo", 201, kMeta) == kInval);
    ps_write32(base, kMeta, 1);
    ps_write32(base, kMeta + 32, 7); // tags: six at most
    CHECK(a.call("cellRecSetInfo", 201, kMeta) == kInval);
    ps_write32(base, kMeta + 32, 1);
    ps_write32(base, kMeta + 36, kTags);
    ps_write32(base, kTags, 0);
    CHECK(a.call("cellRecSetInfo", 201, kMeta) == kInval); // a null tag
    ps_write32(base, kTags, kText2);
    ps_write64(base, kMeta + 16, 1000); // the scene ends after a second
    CHECK(a.call("cellRecSetInfo", 201, kMeta) == 0);
    CHECK(a.call("cellRecSetInfo", 300, 0) == kInval);

    // Close: the scene ends past what was recorded (nothing): ERR with FILE_NO_DATA; then closed.
    REQUIRE(a.call("cellRecClose", 0) == 0);
    CHECK(take(a, base) == std::vector<Entry>{{10, kNoData, kUser}});
    CHECK(a.call("cellRecClose", 0) == kState);
    CHECK(a.call("cellRecSetInfo", 100, 0) == kState);
    CHECK(a.call("cellRecStart") == kState);

    // Opened again with an external RGBA input: the frame's size for the format (MPEG-4 large, 16:9); a discarded
    // or empty close reports CLOSE.
    put_param(base, kParam, 0x0200, 0, kOpts, {{9, 4}});
    REQUIRE(open() == 0);
    CHECK(take(a, base).size() == 1);
    CHECK(info(1) == 640);
    CHECK(info(2) == 2560);
    CHECK(info(3) == 368);
    REQUIRE(a.call("cellRecClose", 1) == 0);
    CHECK(take(a, base) == std::vector<Entry>{{4, 0, kUser}});
    REQUIRE(open() == 0);
    CHECK(take(a, base).size() == 1);
    REQUIRE(a.call("cellRecClose", 0) == 0);
    CHECK(take(a, base) == std::vector<Entry>{{4, 0, kUser}});

    // No container: only when no memory is needed (MJPEG small, reduced).
    put_param(base, kParam, 0x3060, 0x2008, kOpts, {{12, 1}});
    REQUIRE(open(0xFFFFFFFF) == 0);
    CHECK(take(a, base).size() == 1);
    CHECK(info(0) == 0);
}

TEST_CASE("cellSubDisplay: no device registered; parameters and memory as RPCS3 checks them", "[hle][rec]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* base = f.base();
    constexpr u32 kParam = kData + 0x100, kInfo = kData + 0x200, kNum = kData + 0x300, kOut = kData + 0x310;
    const u64 kInval = rec_code(0x80029854), kNotInit = rec_code(0x80029855), kNotSupported = rec_code(0x80029856),
              kZero = rec_code(0x80029813);
    // A CellSubDisplayParam: version, remote play, one group and peer, the video and audio parameters.
    const auto put_sub = [&](s32 version, s32 format, s32 width, s32 height, s32 pitch) {
        const s32 v[] = {version, 1, 1, 1, format, width, height, pitch, 0, 0, 2, 0};
        for (u32 i = 0; i < 12; i++) ps_write32(base, kParam + 4 * i, static_cast<u32>(v[i]));
    };

    put_sub(1, 1, 480, 272, 1920);
    CHECK(a.call("cellSubDisplayGetRequiredMemory", kParam) == 8u * 1024 * 1024);
    put_sub(2, 2, 480, 272, 2048);
    CHECK(a.call("cellSubDisplayGetRequiredMemory", kParam) == 10u * 1024 * 1024);
    put_sub(3, 1, 480, 272, 1920);
    CHECK(a.call("cellSubDisplayGetRequiredMemory", kParam) == kInval); // version 3 is YUV only
    put_sub(3, 3, 864, 480, 864);
    CHECK(a.call("cellSubDisplayGetRequiredMemory", kParam) == 10u * 1024 * 1024);
    put_sub(1, 1, 480, 272, 1000);
    CHECK(a.call("cellSubDisplayGetRequiredMemory", kParam) == kInval);
    CHECK(a.call("cellSubDisplayGetRequiredMemory", 0) == kInval);

    // Init keeps the handler and finds no device; nothing ever starts.
    put_sub(1, 1, 480, 272, 1920);
    CHECK(a.call("cellSubDisplayInit", 0, kParam, 0, 0) == kInval);
    CHECK(a.call("cellSubDisplayInit", kParam, 0, 0, 0) == kInval);
    CHECK(a.call("cellSubDisplayInit", kParam, kParam, 0x55, 0) == kZero);
    CHECK(a.call("cellSubDisplayInit", kParam, kParam, 0x55, 0) == kNotInit); // the handler is kept (RPCS3)
    CHECK(a.call("cellSubDisplayStart") == kNotInit);
    CHECK(a.call("cellSubDisplayStop") == kNotInit);
    CHECK(a.call("cellSubDisplayGetPeerNum", 0) == kNotInit);
    CHECK(a.call("cellSubDisplayGetPeerList", 0, kInfo, kNum) == kNotInit);
    CHECK(a.call("cellSubDisplayGetVideoBuffer", 0, kOut, kNum) == kNotInit);
    CHECK(a.call("cellSubDisplayAudioOutBlocking", 0, kInfo, 1024) == kNotInit);
    CHECK(a.call("cellSubDisplayAudioOutNonBlocking", 0, kInfo, 1024) == kNotInit);
    ps_write32(base, kNum, 3);
    CHECK(a.call("cellSubDisplayGetTouchInfo", 1, kInfo, kNum) == kInval);
    CHECK(a.call("cellSubDisplayGetTouchInfo", 0, kInfo, kNum) == kNotSupported); // version 1
    REQUIRE(a.call("cellSubDisplayEnd") == 0);
    CHECK(a.call("cellSubDisplayEnd") == kNotInit);

    // Version 3: touch information, at most six entries, all empty.
    put_sub(3, 3, 864, 480, 864);
    CHECK(a.call("cellSubDisplayInit", kParam, kParam, 0x55, 0) == kZero);
    std::memset(base + kInfo, 0xAA, 64);
    ps_write32(base, kNum, 8);
    REQUIRE(a.call("cellSubDisplayGetTouchInfo", 0, kInfo, kNum) == 0);
    CHECK(ps_read32(base, kNum) == 6);
    for (u32 i = 0; i < 36; i++) CHECK(base[kInfo + i] == 0);
    CHECK(base[kInfo + 36] == 0xAA);
}

TEST_CASE("fs_utility_init_6B5896B0: two partitions, as RPCS3", "[hle][rec]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* base = f.base();
    ps_write64(base, kData, 0);
    REQUIRE(a.call("fs_utility_init_6B5896B0", kData) == 0);
    CHECK(ps_read64(base, kData) == 2);
    CHECK(a.call("fs_utility_init_6B5896B0", 0) == rec_code(0x8001000D)); // CELL_EFAULT
}
