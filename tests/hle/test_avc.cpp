// AV chat: cellSysutilAvc2 and cellSysutilAvcExt (synthetic: no game data). Loading, joining, leaving and unloading
// through the game's callback, the settings, RPCS3's container size estimate, and the window calls' checks (D-187);
// the functions RPCS3 left as stubs, pspocket's own (D-675 to D-678): the video chat's windows and screen, streaming in
// a chat with nobody else in it, the target, priority and detections, AvcExt's panel, windows and name plates.

#include <catch_amalgamated.hpp>

#include <bit>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "hle_test_util.hpp"
#include "pspocket/hle/hle.hpp"
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

// A guest callback that logs the low words of r3..r6 (kLog: the count, then 16 bytes an entry).
constexpr u32 kCode = kData + 0x20000, kOpd = kData + 0x20800, kLog = kData + 0x21000;

void write_callback(u8* base)
{
    const std::vector<u32> code = {
        as::lis(7, static_cast<s32>(kLog >> 16)), as::ori(7, 7, kLog & 0xFFFF), as::lwz(8, 0, 7), as::slwi(9, 8, 4),
        as::addi(9, 9, 16), as::stwx(3, 7, 9), as::addi(10, 9, 4), as::stwx(4, 7, 10), as::addi(10, 9, 8),
        as::stwx(5, 7, 10), as::addi(10, 9, 12), as::stwx(6, 7, 10), as::addi(8, 8, 1), as::stw(8, 0, 7), as::blr(),
    };
    for (usize i = 0; i < code.size(); i++) ps_write32(base, kCode + 4 * static_cast<u32>(i), code[i]);
    ps_write32(base, kOpd, kCode);
    ps_write32(base, kOpd + 4, 0x1234);
    std::memset(base + kLog, 0, 0x400);
}

struct Entry {
    u32 r3, r4, r5, r6;
    bool operator==(const Entry&) const = default;
};

// The callbacks run by a cellSysutilCheckCallback (registers past `args` read as 0), the log then cleared.
std::vector<Entry> take(GuestThread& g, u8* base, u32 args)
{
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    std::vector<Entry> v;
    for (u32 i = 0; i < ps_read32(base, kLog); i++) {
        const u32 at = kLog + 16 + 16 * i;
        const auto reg = [&](u32 n) { return n < args ? ps_read32(base, at + 4 * n) : 0u; };
        v.push_back({reg(0), reg(1), reg(2), reg(3)});
    }
    std::memset(base + kLog, 0, 0x400);
    return v;
}

void put(u8* base, u32 addr, std::string_view s)
{
    std::memcpy(base + addr, s.data(), s.size());
    base[addr + s.size()] = 0;
}

u64 code(u32 c) { return r3_of_code(c); }

const u64 kInval = code(0x8002B705), kNotInit = code(0x8002B703), kAlready = code(0x8002B704),
          kNotSupported = code(0x8002B702), kUnknown = code(0x8002B701);

// A CellSysutilAvc2InitParam (132 bytes) for a voice chat: version, players, speakers, the streaming mode.
void put_voice_param(u8* b, u32 at, u16 version, u16 players, u16 speakers, u16 streaming = 0, u8 mic_sharing = 0)
{
    std::memset(b + at, 0, 132);
    ps_write16(b, at, version);
    ps_write16(b, at + 2, players);
    ps_write16(b, at + 4, 50);         // spu_load_average
    ps_write16(b, at + 6, streaming);  // direct_streaming_mode
    ps_write32(b, at + 40, 1);         // media_type: voice
    ps_write32(b, at + 44, 1);         // voice_quality: normal
    ps_write16(b, at + 48, speakers);  // max_speakers
    b[at + 50] = mic_sharing;          // mic_out_stream_sharing
}

// The same for a video chat: frame mode, resolution, windows, frame rate, bitrate, sharing.
void put_video_param(u8* b, u32 at, u32 frame_mode, u32 resolution, u16 windows, u16 framerate = 30,
    u32 bitrate = 64000, u8 sharing = 0)
{
    std::memset(b + at, 0, 132);
    ps_write16(b, at, 140);
    ps_write32(b, at + 40, 0x10);
    ps_write32(b, at + 80, frame_mode);
    ps_write32(b, at + 84, resolution);
    ps_write16(b, at + 88, windows);
    ps_write16(b, at + 90, framerate);
    ps_write32(b, at + 92, bitrate);
    b[at + 100] = sharing;
}

} // namespace

TEST_CASE("cellSysutilAvc2: load, join, leave and unload through the callback, as RPCS3 answers them",
    "[hle][avc]")
{
    InterpretMisses interpret;
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* base = f.base();
    write_callback(base);
    constexpr u32 kParam = kData + 0x100, kRoom = kData + 0x200, kArg = 0x5151;
    ps_write64(base, kRoom, 0x0001000200030004ull);

    // cellSysutilAvc2InitParam: cleared, the version written, then checked.
    std::memset(base + kParam, 0xAA, 132);
    REQUIRE(a.call("cellSysutilAvc2InitParam", 140, kParam) == 0);
    CHECK(ps_read16(base, kParam) == 140);
    for (u32 i = 2; i < 132; i++) CHECK(base[kParam + i] == 0);
    CHECK(a.call("cellSysutilAvc2InitParam", 99, kParam) == kInval);
    CHECK(ps_read16(base, kParam) == 99);
    CHECK(a.call("cellSysutilAvc2InitParam", 140, 0) == kInval);

    // A voice chat's parameters as RPCS3 checks them; the callback last.
    const auto load = [&](u32 callback) { return a.call("cellSysutilAvc2Load", 1, 0, callback, kArg, kParam); };
    CHECK(a.call("cellSysutilAvc2Load", 1, 0, kOpd, kArg, 0) == kInval);
    put_voice_param(base, kParam, 140, 1, 4);
    CHECK(load(kOpd) == kInval); // 2 to 64 players
    put_voice_param(base, kParam, 140, 65, 4);
    CHECK(load(kOpd) == kInval);
    put_voice_param(base, kParam, 140, 8, 0);
    CHECK(load(kOpd) == kInval); // 1 to 16 speakers
    put_voice_param(base, kParam, 140, 8, 17);
    CHECK(load(kOpd) == kInval);
    put_voice_param(base, kParam, 140, 8, 4, 3);
    CHECK(load(kOpd) == kInval); // no such streaming mode
    put_voice_param(base, kParam, 110, 8, 4, 2);
    CHECK(load(kOpd) == kInval); // direct LAN from version 120 on
    put_voice_param(base, kParam, 105, 8, 4);
    CHECK(load(kOpd) == kInval); // an unknown version
    put_voice_param(base, kParam, 140, 8, 4);
    ps_write32(base, kParam + 44, 2);
    CHECK(load(kOpd) == kInval); // normal quality only
    put_voice_param(base, kParam, 140, 8, 4);
    ps_write32(base, kParam + 40, 2);
    CHECK(load(kOpd) == kNotSupported); // neither voice nor video
    put_voice_param(base, kParam, 140, 8, 4);
    CHECK(load(0) == kInval);
    CHECK(a.call("cellSysutilAvc2Load", 1, 0, kOpd, kArg, 0x0FFF0000) == kInval); // RPCS3 faults
    REQUIRE(load(kOpd) == 0);
    CHECK(load(kOpd) == kAlready);
    CHECK(take(a, base, 3).empty()); // a synchronous load reports nothing

    // Joining and leaving: requests succeed through the callback (RPCS3's TODOs); a room is optional in normal mode.
    CHECK(a.call("cellSysutilAvc2JoinChat", 0, 0, 0) == 0);
    REQUIRE(a.call("cellSysutilAvc2JoinChatRequest", kRoom) == 0);
    REQUIRE(a.call("cellSysutilAvc2LeaveChatRequest") == 0);
    CHECK(take(a, base, 3) == std::vector<Entry>{{5, 0, kArg, 0}, {7, 0, kArg, 0}});

    // Unload drops the callback at once; a second has nothing to drop.
    REQUIRE(a.call("cellSysutilAvc2Unload") == 0);
    CHECK(a.call("cellSysutilAvc2Unload") == kNotInit);
    REQUIRE(a.call("cellSysutilAvc2LeaveChatRequest") == 0);
    CHECK(take(a, base, 3).empty());

    // LoadAsync reports LOAD_SUCCEEDED, unless an unload comes first (RPCS3 unregisters the waiting events).
    REQUIRE(a.call("cellSysutilAvc2LoadAsync", 1, 0, kOpd, kArg, kParam) == 0);
    REQUIRE(a.call("cellSysutilAvc2Unload") == 0);
    CHECK(take(a, base, 3).empty());
    REQUIRE(a.call("cellSysutilAvc2LoadAsync", 1, 0, kOpd, kArg, kParam) == 0);
    CHECK(take(a, base, 3) == std::vector<Entry>{{1, 0, kArg, 0}});

    // UnloadAsync: UNLOAD_SUCCEEDED, after which the callback and the events still waiting are gone.
    REQUIRE(a.call("cellSysutilAvc2UnloadAsync") == 0);
    REQUIRE(a.call("cellSysutilAvc2UnloadAsync") == 0);
    CHECK(take(a, base, 3) == std::vector<Entry>{{3, 0, kArg, 0}});
    CHECK(a.call("cellSysutilAvc2Unload") == kNotInit);

    // UnloadAsync2: the voice chat's event drops the callback; the video chat's carries 2 and keeps it.
    CHECK(a.call("cellSysutilAvc2UnloadAsync2", 5) == kInval);
    REQUIRE(a.call("cellSysutilAvc2LoadAsync", 1, 0, kOpd, kArg, kParam) == 0);
    REQUIRE(a.call("cellSysutilAvc2UnloadAsync2", 0x10) == 0);
    CHECK(take(a, base, 3) == std::vector<Entry>{{1, 0, kArg, 0}, {3, 2, kArg, 0}});
    CHECK(a.call("cellSysutilAvc2Unload2", 3) == kInval);
    CHECK(a.call("cellSysutilAvc2Unload2", 0x10) == 0);
    REQUIRE(a.call("cellSysutilAvc2Unload2", 1) == 0);
    CHECK(a.call("cellSysutilAvc2Unload2", 1) == kNotInit);
    REQUIRE(a.call("cellSysutilAvc2UnloadAsync2", 1) == 0);
    CHECK(take(a, base, 3).empty()); // nothing loaded, nobody to tell

    // A direct streaming mode needs no room, and refuses one that isn't given.
    put_voice_param(base, kParam, 120, 8, 4, 1);
    REQUIRE(load(kOpd) == 0);
    CHECK(a.call("cellSysutilAvc2JoinChat", 0, 0, 0) == kInval);
    CHECK(a.call("cellSysutilAvc2JoinChatRequest", 0) == kInval);
    CHECK(a.call("cellSysutilAvc2JoinChat", kRoom, 0, 0) == 0);
    REQUIRE(a.call("cellSysutilAvc2Unload") == 0);

    // A video chat: no callback (RPCS3's check), up to 6 windows (16 with intra frames only), 1000 to 512000 bit/s,
    // up to 30 frames a second.
    put_video_param(base, kParam, 1, 1, 6);
    CHECK(load(kOpd) == kInval);
    REQUIRE(load(0) == 0);
    put_video_param(base, kParam, 1, 1, 7);
    CHECK(load(0) == kInval);
    put_video_param(base, kParam, 2, 2, 16);
    CHECK(load(0) == 0);
    put_video_param(base, kParam, 1, 1, 0);
    CHECK(load(0) == kInval);
    put_video_param(base, kParam, 1, 1, 4, 31);
    CHECK(load(0) == kInval);
    put_video_param(base, kParam, 1, 1, 4, 30, 999);
    CHECK(load(0) == kInval);
    put_video_param(base, kParam, 1, 1, 4, 30, 512001);
    CHECK(load(0) == kInval);
}

TEST_CASE("cellSysutilAvc2: settings kept, no camera or microphone, one player, RPCS3's size estimate", "[hle][avc]")
{
    InterpretMisses interpret;
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* base = f.base();
    write_callback(base);
    constexpr u32 kByte = kData + 0x100, kFloat = kData + 0x104, kNum = kData + 0x108, kIds = kData + 0x110,
                  kId = kData + 0x130, kInfo = kData + 0x140, kSize = kData + 0x150, kText = kData + 0x200,
                  kParam = kData + 0x300, kBuf = kData + 0x400;
    const auto byte = [&](std::string_view fn) {
        base[kByte] = 0xAA;
        REQUIRE(a.call(fn, kByte) == 0);
        return base[kByte];
    };

    // Muting: voice, video and speaker all on at first; video muting is 0 or 1.
    CHECK(byte("cellSysutilAvc2GetVoiceMuting") == 1);
    REQUIRE(a.call("cellSysutilAvc2SetVoiceMuting", 0) == 0);
    CHECK(byte("cellSysutilAvc2GetVoiceMuting") == 0);
    CHECK(byte("cellSysutilAvc2GetVideoMuting") == 1);
    CHECK(a.call("cellSysutilAvc2SetVideoMuting", 2) == kInval);
    REQUIRE(a.call("cellSysutilAvc2SetVideoMuting", 0) == 0);
    CHECK(byte("cellSysutilAvc2GetVideoMuting") == 0);
    CHECK(byte("cellSysutilAvc2GetSpeakerMuting") == 1);
    REQUIRE(a.call("cellSysutilAvc2SetSpeakerMuting", 0) == 0);
    CHECK(byte("cellSysutilAvc2GetSpeakerMuting") == 0);
    CHECK(a.call("cellSysutilAvc2GetVoiceMuting", 0) == kInval);

    // The speaker volume (40 at first, in f1 when set) and the muted players.
    REQUIRE(a.call("cellSysutilAvc2GetSpeakerVolumeLevel", kFloat) == 0);
    CHECK(std::bit_cast<f32>(ps_read32(base, kFloat)) == 40.0f);
    a.ctx.fpr[1] = 12.5;
    REQUIRE(a.call("cellSysutilAvc2SetSpeakerVolumeLevel") == 0);
    REQUIRE(a.call("cellSysutilAvc2GetSpeakerVolumeLevel", kFloat) == 0);
    CHECK(std::bit_cast<f32>(ps_read32(base, kFloat)) == 12.5f);
    CHECK(a.call("cellSysutilAvc2GetSpeakerVolumeLevel", 0) == kInval);
    REQUIRE(a.call("cellSysutilAvc2GetPlayerVoiceMuting", 3, kByte) == 0);
    CHECK(base[kByte] == 0);
    REQUIRE(a.call("cellSysutilAvc2SetPlayerVoiceMuting", 3, 1) == 0);
    REQUIRE(a.call("cellSysutilAvc2GetPlayerVoiceMuting", 3, kByte) == 0);
    CHECK(base[kByte] == 1);
    REQUIRE(a.call("cellSysutilAvc2SetPlayerVoiceMuting", 3, 0) == 0);
    REQUIRE(a.call("cellSysutilAvc2GetPlayerVoiceMuting", 3, kByte) == 0);
    CHECK(base[kByte] == 0);

    // No camera or microphone; nothing on screen.
    CHECK(byte("cellSysutilAvc2IsCameraAttached") == 0);
    CHECK(byte("cellSysutilAvc2IsMicAttached") == 0);
    CHECK(a.call("cellSysutilAvc2IsMicAttached", 0) == kInval); // RPCS3 aborts
    CHECK(byte("cellSysutilAvc2GetScreenShowStatus") == 0);
    base[kByte] = 0xAA;
    REQUIRE(a.call("cellSysutilAvc2GetWindowShowStatus", 1, kByte) == 0);
    CHECK(base[kByte] == 0);

    // Players: the one asked about, joined and connected without a microphone; the list counts one.
    ps_write16(base, kId, 0x1234);
    std::memset(base + kInfo, 0xAA, 16);
    REQUIRE(a.call("cellSysutilAvc2GetPlayerInfo", kId, kInfo) == 0);
    CHECK(ps_read16(base, kInfo) == 0x1234);
    CHECK(base[kInfo + 2] == 1);
    CHECK(base[kInfo + 3] == 1);
    CHECK(base[kInfo + 4] == 0);
    CHECK(base[kInfo + 5] == 0xAA);
    CHECK(a.call("cellSysutilAvc2GetPlayerInfo", 0, kInfo) == kInval);
    REQUIRE(a.call("cellSysutilAvc2EnumPlayers", kNum, 0) == 0);
    CHECK(ps_read32(base, kNum) == 1);
    ps_write32(base, kNum, 3);
    REQUIRE(a.call("cellSysutilAvc2EnumPlayers", kNum, kIds) == 0);
    CHECK(ps_read16(base, kIds) == 1);
    CHECK(ps_read16(base, kIds + 2) == 2);
    CHECK(ps_read16(base, kIds + 4) == 3);
    CHECK(a.call("cellSysutilAvc2EnumPlayers", 0, kIds) == kInval);

    // The microphone's stream: only when shared, and then empty.
    ps_write32(base, kSize, 100);
    REQUIRE(a.call("cellSysutilAvc2MicRead", kBuf, kSize) == 0);
    CHECK(ps_read32(base, kSize) == 100);
    put_voice_param(base, kParam, 140, 8, 4, 0, 1);
    REQUIRE(a.call("cellSysutilAvc2Load", 1, 0, kOpd, 0, kParam) == 0);
    REQUIRE(a.call("cellSysutilAvc2MicRead", kBuf, kSize) == 0);
    CHECK(ps_read32(base, kSize) == 0);
    CHECK(a.call("cellSysutilAvc2MicRead", 0, 0) == 0); // not checked on the PS3 (RPCS3)

    // Window strings shorter than 64 bytes; the rest checks pointers only.
    put(base, kText, "hello");
    CHECK(a.call("cellSysutilAvc2SetWindowString", 1, kText) == 0);
    put(base, kText, std::string(64, 'x'));
    CHECK(a.call("cellSysutilAvc2SetWindowString", 1, kText) == kInval);
    CHECK(a.call("cellSysutilAvc2SetWindowString", 1, 0) == kInval);
    CHECK(a.call("cellSysutilAvc2GetAttribute", 0) == kInval);
    CHECK(a.call("cellSysutilAvc2SetAttribute", kBuf) == 0);
    CHECK(a.call("cellSysutilAvc2GetWindowAttribute", 1, 0) == kInval);
    CHECK(a.call("cellSysutilAvc2SetWindowAttribute", 1, kBuf) == 0);
    CHECK(a.call("cellSysutilAvc2GetWindowSize", 1, kBuf, 0) == kInval);
    CHECK(a.call("cellSysutilAvc2GetWindowSize", 1, kBuf, kBuf + 4) == 0);
    CHECK(a.call("cellSysutilAvc2GetWindowPosition", 1, kBuf, kBuf + 4, 0) == kInval);
    CHECK(a.call("cellSysutilAvc2GetWindowString", 1, kText, 0) == kInval);
    CHECK(a.call("cellSysutilAvc2StartStreaming2", 1) == 0);
    CHECK(a.call("cellSysutilAvc2StartStreaming2", 2) == kInval);
    CHECK(a.call("cellSysutilAvc2StopStreaming2", 0x10) == 0);

    // RPCS3's estimate of the memory container: 4 MB for version 100, 3 MB for a voice chat, a video chat's from its
    // windows, resolution and frame mode (values from RPCS3's arithmetic).
    const auto estimate = [&]() -> u64 {
        ps_write32(base, kSize, 0xAAAAAAAA);
        return a.call("cellSysutilAvc2EstimateMemoryContainerSize", kParam, kSize);
    };
    put_voice_param(base, kParam, 100, 8, 4);
    REQUIRE(estimate() == 0);
    CHECK(ps_read32(base, kSize) == 0x400000);
    put_voice_param(base, kParam, 130, 8, 4);
    REQUIRE(estimate() == 0);
    CHECK(ps_read32(base, kSize) == 0x300000);
    struct Video {
        u32 resolution, windows, sharing, frame_mode, size;
    };
    for (const Video& v : {Video{1, 6, 0, 1, 0xA00000}, Video{2, 2, 2, 2, 0x900000}, Video{2, 6, 0, 1, 0xF00000},
             Video{1, 16, 2, 2, 0x900000}, Video{3, 4, 0, 1, 0xB00000}}) {
        INFO(v.resolution << " " << v.windows << " " << v.sharing << " " << v.frame_mode);
        put_video_param(base, kParam, v.frame_mode, v.resolution, static_cast<u16>(v.windows), 30, 64000,
            static_cast<u8>(v.sharing));
        REQUIRE(estimate() == 0);
        CHECK(ps_read32(base, kSize) == v.size);
    }
    put_voice_param(base, kParam, 140, 8, 4);
    ps_write32(base, kParam + 40, 3);
    CHECK(estimate() == kInval);
    CHECK(ps_read32(base, kSize) == 0);
    put_voice_param(base, kParam, 99, 8, 4);
    CHECK(estimate() == kInval);
    CHECK(ps_read32(base, kSize) == 0xAAAAAAAAu);
    CHECK(a.call("cellSysutilAvc2EstimateMemoryContainerSize", kParam, 0) == kInval);
}

TEST_CASE("cellSysutilAvcExt: windows checked, nothing shown; options; LoadAsyncEx loads cellSysutilAvc",
    "[hle][avc]")
{
    InterpretMisses interpret;
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* base = f.base();
    write_callback(base);
    constexpr u32 kStatus = kData + 0x100, kNp = kData + 0x200, kOut = kData + 0x300, kOpt = kData + 0x400,
                  kReq = kData + 0x500, kUser = 0x77;

    // RPCS3 writes nothing for the attached devices (and aborts on a null pointer).
    ps_write32(base, kStatus, 0xAAAAAAAA);
    CHECK(a.call("cellSysutilAvcExtIsMicAttached", kStatus) == 0);
    CHECK(a.call("cellSysutilAvcExtIsCameraAttached", kStatus) == 0);
    CHECK(ps_read32(base, kStatus) == 0xAAAAAAAAu);
    CHECK(a.call("cellSysutilAvcExtIsMicAttached", 0) == kInval);

    // Getters check their pointers.
    CHECK(a.call("cellSysutilAvcExtGetWindowPosition", kNp, kOut, kOut + 4, kOut + 8) == 0);
    CHECK(a.call("cellSysutilAvcExtGetWindowPosition", 0, kOut, kOut + 4, kOut + 8) == kInval);
    CHECK(a.call("cellSysutilAvcExtGetWindowSize", kNp, kOut, 0) == kInval);
    CHECK(a.call("cellSysutilAvcExtGetWindowShowStatus", kNp, kOut) == 0);
    CHECK(a.call("cellSysutilAvcExtGetNamePlateShowStatus", 0) == kInval);
    CHECK(a.call("cellSysutilAvcExtGetWindowRotation", kNp, kOut, kOut, 0) == kInval);
    CHECK(a.call("cellSysutilAvcExtGetWindowAlpha", kNp, kOut) == 0);
    CHECK(a.call("cellSysutilAvcExtGetSurfacePointer", kNp, kOut, kOut, kOut, 0) == kInval);
    CHECK(a.call("cellSysutilAvcExtGetSurfacePointer", kNp, kOut, kOut, kOut, kOut) == 0);

    // Setters: a player and a transition up to EXPONENT (NONE, 0xFFFFFFFF, is refused as in RPCS3); a z-order. The
    // transition after the floats comes from the register after their slots (D-678): r5 after one, r6 after two.
    a.ctx.fpr[1] = 0.5;
    CHECK(a.call("cellSysutilAvcExtSetWindowAlpha", kNp, 0, 4) == 0);
    CHECK(a.call("cellSysutilAvcExtSetWindowAlpha", kNp, 0, 5) == kInval);
    CHECK(a.call("cellSysutilAvcExtSetWindowAlpha", kNp, 0, 0xFFFFFFFF) == kInval);
    CHECK(a.call("cellSysutilAvcExtSetWindowAlpha", 0, 0, 0) == kInval);
    CHECK(a.call("cellSysutilAvcExtSetWindowSize", kNp, 0, 0, 0) == 0);
    CHECK(a.call("cellSysutilAvcExtShowWindow", kNp, 1) == 0);
    CHECK(a.call("cellSysutilAvcExtHideWindow", 0, 1) == kInval);
    CHECK(a.call("cellSysutilAvcExtSetWindowZorder", kNp, 1) == kInval);
    CHECK(a.call("cellSysutilAvcExtSetWindowZorder", kNp, 2) == 0);
    CHECK(a.call("cellSysutilAvcExtSetWindowZorder", kNp, 3) == 0);
    CHECK(a.call("cellSysutilAvcExtSetWindowZorder", kNp, 4) == kInval);

    // The option: version 100, or 180 with 16 players; another version is written, then ERROR_UNKNOWN.
    std::memset(base + kOpt, 0xAA, 12);
    REQUIRE(a.call("cellSysutilAvcExtInitOptionParam", 100, kOpt) == 0);
    CHECK(ps_read32(base, kOpt) == 100);
    CHECK(base[kOpt + 4] == 0);
    CHECK(ps_read32(base, kOpt + 8) == 0xAAAAAAAAu);
    REQUIRE(a.call("cellSysutilAvcExtInitOptionParam", 180, kOpt) == 0);
    CHECK(ps_read32(base, kOpt + 8) == 16);
    CHECK(a.call("cellSysutilAvcExtInitOptionParam", 5, kOpt) == kUnknown);
    CHECK(ps_read32(base, kOpt) == 5);
    CHECK(a.call("cellSysutilAvcExtInitOptionParam", 100, 0) == kInval);

    // LoadAsyncEx: the option (no shared video buffer for a voice chat), then cellSysutilAvcLoadAsync, which tells the
    // callback (requestId, eventId, eventParam, userdata) at the next check.
    const auto load = [&] { return a.call("cellSysutilAvcExtLoadAsyncEx", kOpd, kUser, 0, 1, 1, 1, kOpt, kReq); };
    CHECK(a.call("cellSysutilAvcExtLoadAsyncEx", kOpd, kUser, 0, 1, 1, 1, 0, kReq) == kInval);
    CHECK(load() == kUnknown);
    REQUIRE(a.call("cellSysutilAvcExtInitOptionParam", 100, kOpt) == 0);
    base[kOpt + 4] = 1;
    CHECK(load() == kInval);
    base[kOpt + 4] = 0;
    ps_write32(base, kReq, 0xAAAAAAAA);
    REQUIRE(load() == 0);
    const u32 request = ps_read32(base, kReq);
    CHECK(request != 0xAAAAAAAAu);
    CHECK(load() == kAlready);
    CHECK(take(a, base, 4) == std::vector<Entry>{{request, 1, 0, kUser}});
}

namespace {

const u64 kInvalStatus = code(0x8002B70A), kExists = code(0x8002B70F), kTooMany = code(0x8002B710),
          kNotFound = code(0x8002B712);
constexpr u32 kUnmapped = 0x0FFF0000; // not guest memory in the fixture

f32 float_at(u8* base, u32 addr) { return std::bit_cast<f32>(ps_read32(base, addr)); }

// The functions RPCS3 leaves as stubs in `module` (D-182): each now pspocket's own under its name, unverified (D-594).
usize own_stubs(std::string_view module)
{
    usize n = 0;
    for (const hle::StubFunction& s : hle::rpcs3_stubs()) {
        if (s.module != module) continue;
        INFO(s.module << "::" << s.name);
        const hle::Function* fn = hle::registry().find(s.module, s.nid);
        REQUIRE(fn != nullptr);
        CHECK(fn->name == s.name);
        CHECK(hle::registry().is_unverified(s.module, s.nid));
        n++;
    }
    return n;
}

} // namespace

// D-678, a regression: the transition after SetWindowAlpha's and SetWindowSize's floats was read from r4, the first
// float's slot (RPCS3's binder), where the caller leaves whatever r4 held: a good transition was refused by what was
// there, a bad one taken. It comes from r5 after one float, r6 after two.
TEST_CASE("cellSysutilAvcExt: the transition after float arguments comes from the register after their slots (D-678)",
    "[hle][avc][avcext]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    constexpr u32 kNp = kData + 0x200;
    a.ctx.fpr[1] = 0.75;
    a.ctx.fpr[2] = 0.5;
    constexpr u64 kGarbage = 0xDEADBEEF; // r4, r5: what the caller left in the floats' slots
    CHECK(a.call("cellSysutilAvcExtSetWindowAlpha", kNp, kGarbage, 1) == 0);
    CHECK(a.call("cellSysutilAvcExtSetWindowAlpha", kNp, 0, 5) == kInval);
    CHECK(a.call("cellSysutilAvcExtSetWindowSize", kNp, kGarbage, kGarbage, 3) == 0);
    CHECK(a.call("cellSysutilAvcExtSetWindowSize", kNp, 0, 0, 5) == kInval);
}

TEST_CASE("AV chat: every stub RPCS3 left is pspocket's own, unverified (D-675 to D-677)", "[hle][avc][avc2][avcext]")
{
    CHECK(own_stubs("cellSysutilAvc2") == 16);
    CHECK(own_stubs("cellSysutilAvcExt") == 14);
}

TEST_CASE("cellSysutilAvcExt: the panel is libsysutil's request, refused with no system software (D-677)",
    "[hle][avc][avcext]")
{
    InterpretMisses interpret;
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* base = f.base();
    write_callback(base);
    constexpr u32 kReq = kData + 0x100;
    const auto load = [&](u32 media) { return a.call("cellSysutilAvcLoadAsync", kOpd, 0x55, 0, media, 1, 1, kReq); };

    // No AV chat: INVALID_STATUS, after the transition's check (NONE, -1, passes it, as libsysutil's export has it).
    CHECK(a.call("cellSysutilAvcExtShowPanelEx", 0) == kInvalStatus);
    CHECK(a.call("cellSysutilAvcExtHidePanelEx", 0xFFFFFFFF) == kInvalStatus);
    CHECK(a.call("cellSysutilAvcExtShowPanelEx", 5) == kInval);
    // A voice chat has no panel.
    REQUIRE(load(1) == 0);
    CHECK(a.call("cellSysutilAvcExtShowPanelEx", 0) == kInvalStatus);
    REQUIRE(a.call("cellSysutilAvcUnloadAsync", kReq) == 0);
    REQUIRE(take(a, base, 4).size() == 2); // loaded, unloaded
    // A video chat's panel request goes to the system, which is not there: ERROR_UNKNOWN, as cellSysutilAvcShowPanel.
    REQUIRE(load(2) == 0);
    CHECK(a.call("cellSysutilAvcExtShowPanelEx", 0) == kUnknown);
    CHECK(a.call("cellSysutilAvcExtShowPanelEx", 4) == kUnknown);
    CHECK(a.call("cellSysutilAvcExtHidePanelEx", 0xFFFFFFFF) == kUnknown);
    CHECK(a.call("cellSysutilAvcExtHidePanelEx", 7) == kInval);
    CHECK(a.call("cellSysutilAvcShowPanel") == kUnknown);
}

TEST_CASE("cellSysutilAvcExt: each player's window kept as the game set it, name plates, chat mode and group, "
          "detections with no device (D-677)",
    "[hle][avc][avcext]")
{
    InterpretMisses interpret;
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* base = f.base();
    write_callback(base);
    constexpr u32 kAlice = kData + 0x100, kAlice2 = kData + 0x140, kBob = kData + 0x180, kOut = kData + 0x200,
                  kReq = kData + 0x240;
    constexpr u64 kGarbage = 0xDEADBEEF; // what the caller left in the floats' registers
    const auto np_id = [&](u32 at, std::string_view online_id, u8 fill) {
        std::memset(base + at, fill, 36); // after the online ID's NUL: the term's padding, opt, reserved
        std::memcpy(base + at, online_id.data(), online_id.size());
        base[at + online_id.size()] = 0;
    };
    np_id(kAlice, "alice", 0);
    np_id(kAlice2, "alice", 0x5A); // the same online ID, other bytes after it: the same player
    np_id(kBob, "bob", 0);
    const auto floats3 = [&](std::string_view fn, u32 player, double x, double y, double z, u64 transition) {
        a.ctx.fpr[1] = x;
        a.ctx.fpr[2] = y;
        a.ctx.fpr[3] = z;
        return a.call(fn, player, kGarbage, kGarbage, kGarbage, transition); // the transition in r7
    };
    const auto blank = [&] { std::memset(base + kOut, 0xAA, 16); };

    // Nothing set yet: the getters write nothing.
    blank();
    REQUIRE(a.call("cellSysutilAvcExtGetWindowPosition", kAlice, kOut, kOut + 4, kOut + 8) == 0);
    REQUIRE(a.call("cellSysutilAvcExtGetWindowRotation", kAlice, kOut, kOut + 4, kOut + 8) == 0);
    REQUIRE(a.call("cellSysutilAvcExtGetWindowSize", kAlice, kOut, kOut + 4) == 0);
    REQUIRE(a.call("cellSysutilAvcExtGetWindowAlpha", kAlice, kOut) == 0);
    REQUIRE(a.call("cellSysutilAvcExtGetWindowShowStatus", kAlice, kOut) == 0);
    REQUIRE(a.call("cellSysutilAvcExtGetNamePlateShowStatus", kOut) == 0);
    CHECK(ps_read32(base, kOut) == 0xAAAAAAAAu);
    CHECK(ps_read32(base, kOut + 8) == 0xAAAAAAAAu);

    // Position and rotation, checked as SetWindowAlpha; the transition after the three floats from r7.
    REQUIRE(floats3("cellSysutilAvcExtSetWindowPosition", kAlice, 0.25, -0.75, 2.0, 2) == 0);
    REQUIRE(floats3("cellSysutilAvcExtSetWindowRotation", kBob, 10.0, 20.0, 30.0, 0) == 0);
    CHECK(floats3("cellSysutilAvcExtSetWindowPosition", kAlice, 0, 0, 0, 5) == kInval);
    CHECK(floats3("cellSysutilAvcExtSetWindowRotation", kBob, 0, 0, 0, 0xFFFFFFFF) == kInval);
    CHECK(floats3("cellSysutilAvcExtSetWindowPosition", 0, 0, 0, 0, 0) == kInval);
    CHECK(floats3("cellSysutilAvcExtSetWindowRotation", kUnmapped, 0, 0, 0, 0) == kInval); // RPCS3 reads nothing
    REQUIRE(a.call("cellSysutilAvcExtGetWindowPosition", kAlice2, kOut, kOut + 4, kOut + 8) == 0);
    CHECK(float_at(base, kOut) == 0.25f);
    CHECK(float_at(base, kOut + 4) == -0.75f);
    CHECK(float_at(base, kOut + 8) == 2.0f);
    REQUIRE(a.call("cellSysutilAvcExtGetWindowRotation", kBob, kOut, kOut + 4, kOut + 8) == 0);
    CHECK(float_at(base, kOut) == 10.0f);
    CHECK(float_at(base, kOut + 8) == 30.0f);
    blank();
    REQUIRE(a.call("cellSysutilAvcExtGetWindowRotation", kAlice, kOut, kOut + 4, kOut + 8) == 0); // Alice's: none
    REQUIRE(a.call("cellSysutilAvcExtGetWindowPosition", kBob, kOut, kOut + 4, kOut + 8) == 0);
    CHECK(ps_read32(base, kOut) == 0xAAAAAAAAu);

    // Alpha, size and the show status as D-187's setters now keep them.
    a.ctx.fpr[1] = 0.5;
    REQUIRE(a.call("cellSysutilAvcExtSetWindowAlpha", kAlice, kGarbage, 1) == 0);
    REQUIRE(a.call("cellSysutilAvcExtGetWindowAlpha", kAlice, kOut) == 0);
    CHECK(float_at(base, kOut) == 0.5f);
    a.ctx.fpr[1] = 320.0;
    a.ctx.fpr[2] = 240.0;
    REQUIRE(a.call("cellSysutilAvcExtSetWindowSize", kAlice, kGarbage, kGarbage, 0) == 0);
    REQUIRE(a.call("cellSysutilAvcExtGetWindowSize", kAlice, kOut, kOut + 4) == 0);
    CHECK(float_at(base, kOut) == 320.0f);
    CHECK(float_at(base, kOut + 4) == 240.0f);
    REQUIRE(a.call("cellSysutilAvcExtShowWindow", kAlice, 0) == 0);
    REQUIRE(a.call("cellSysutilAvcExtGetWindowShowStatus", kAlice, kOut) == 0);
    CHECK(base[kOut] == 1);
    REQUIRE(a.call("cellSysutilAvcExtHideWindow", kAlice2, 0) == 0);
    REQUIRE(a.call("cellSysutilAvcExtGetWindowShowStatus", kAlice, kOut) == 0);
    CHECK(base[kOut] == 0);
    base[kOut] = 0xAA;
    REQUIRE(a.call("cellSysutilAvcExtGetWindowShowStatus", kBob, kOut) == 0);
    CHECK(base[kOut] == 0xAA);
    REQUIRE(a.call("cellSysutilAvcExtSetWindowZorder", kBob, 3) == 0);
    CHECK(a.call("cellSysutilAvcExtSetWindowZorder", kUnmapped, 3) == kInval);
    // Outputs that are not guest memory, once there is something to write.
    CHECK(a.call("cellSysutilAvcExtGetWindowAlpha", kAlice, kUnmapped) == kInval);
    CHECK(a.call("cellSysutilAvcExtGetWindowShowStatus", kAlice, kUnmapped) == kInval);
    CHECK(a.call("cellSysutilAvcExtGetWindowPosition", kAlice, kOut, kOut + 4, 0) == kInval);
    CHECK(a.call("cellSysutilAvcExtGetWindowPosition", kUnmapped, kOut, kOut + 4, kOut + 8) == kInval);

    // The name plates, as the game last said.
    REQUIRE(a.call("cellSysutilAvcExtSetHideNamePlate") == 0);
    REQUIRE(a.call("cellSysutilAvcExtGetNamePlateShowStatus", kOut) == 0);
    CHECK(base[kOut] == 0);
    REQUIRE(a.call("cellSysutilAvcExtSetShowNamePlate") == 0);
    REQUIRE(a.call("cellSysutilAvcExtGetNamePlateShowStatus", kOut) == 0);
    CHECK(base[kOut] == 1);
    CHECK(a.call("cellSysutilAvcExtGetNamePlateShowStatus", kUnmapped) == kInval);

    // The chat mode and group; the detections, with the AV chat loaded: no device, so no event ever comes.
    CHECK(a.call("cellSysutilAvcExtSetChatMode", 2) == 0);
    CHECK(a.call("cellSysutilAvcExtSetChatGroup") == 0);
    REQUIRE(a.call("cellSysutilAvcLoadAsync", kOpd, 0x55, 0, 2, 1, 1, kReq) == 0);
    for (const char* fn : {"cellSysutilAvcExtStartCameraDetection", "cellSysutilAvcExtStartMicDetection",
             "cellSysutilAvcExtStartVoiceDetection", "cellSysutilAvcExtStopCameraDetection",
             "cellSysutilAvcExtStopMicDetection", "cellSysutilAvcExtStopVoiceDetection",
             "cellSysutilAvcExtStartVoiceDetection"}) {
        INFO(fn);
        CHECK(a.call(fn) == 0);
    }
    CHECK(take(a, base, 4) == std::vector<Entry>{{ps_read32(base, kReq), 1, 0, 0x55}}); // the load's only
}

TEST_CASE("cellSysutilAvc2: the video chat's windows and screen, the game's own (D-676)", "[hle][avc][avc2]")
{
    InterpretMisses interpret;
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* base = f.base();
    write_callback(base);
    constexpr u32 kParam = kData + 0x100, kOut = kData + 0x200, kByte = kData + 0x220;
    const auto load = [&](u32 callback) { return a.call("cellSysutilAvc2Load", 1, 0, callback, 0, kParam); };
    const auto window_shown = [&](u32 member) {
        base[kByte] = 0xAA;
        REQUIRE(a.call("cellSysutilAvc2GetWindowShowStatus", member, kByte) == 0);
        return base[kByte];
    };
    const auto screen_shown = [&] {
        base[kByte] = 0xAA;
        REQUIRE(a.call("cellSysutilAvc2GetScreenShowStatus", kByte) == 0);
        return base[kByte];
    };
    const auto position = [&](u32 member, double x, double y, double z) {
        a.ctx.fpr[1] = x;
        a.ctx.fpr[2] = y;
        a.ctx.fpr[3] = z;
        return a.call("cellSysutilAvc2SetWindowPosition", member);
    };
    const auto size = [&](u32 member, double w, double h) {
        a.ctx.fpr[1] = w;
        a.ctx.fpr[2] = h;
        return a.call("cellSysutilAvc2SetWindowSize", member);
    };

    // Nothing without the video chat; an unknown resolution is refused first.
    CHECK(a.call("cellSysutilAvc2CreateWindow", 5) == kNotInit);
    CHECK(a.call("cellSysutilAvc2ShowWindow", 5) == kNotInit);
    CHECK(a.call("cellSysutilAvc2ShowScreen") == kNotInit);
    CHECK(a.call("cellSysutilAvc2ChangeVideoResolution", 1) == kNotInit);
    CHECK(a.call("cellSysutilAvc2ChangeVideoResolution", 3) == kInval);

    // The voice chat, then a video chat of two QQVGA windows.
    put_voice_param(base, kParam, 140, 8, 4);
    REQUIRE(load(kOpd) == 0);
    CHECK(a.call("cellSysutilAvc2CreateWindow", 5) == kNotInit); // the voice chat has no windows
    put_video_param(base, kParam, 1, 1, 2);
    REQUIRE(load(0) == 0);

    // Windows: one a member, at most max_video_windows; any member ID (no room to check it against).
    REQUIRE(a.call("cellSysutilAvc2CreateWindow", 5) == 0);
    CHECK(a.call("cellSysutilAvc2CreateWindow", 5) == kExists);
    REQUIRE(a.call("cellSysutilAvc2CreateWindow", 0xFFF0) == 0); // the custom video window's ID
    CHECK(a.call("cellSysutilAvc2CreateWindow", 7) == kTooMany);
    CHECK(a.call("cellSysutilAvc2CreateWindow", 0x10005) == kExists); // a 16-bit member ID

    // Shown and hidden; a member with no window is not found (its getter answers hidden, as before).
    CHECK(window_shown(5) == 0);
    REQUIRE(a.call("cellSysutilAvc2ShowWindow", 5) == 0);
    CHECK(window_shown(5) == 1);
    CHECK(window_shown(0xFFF0) == 0);
    REQUIRE(a.call("cellSysutilAvc2HideWindow", 5) == 0);
    CHECK(window_shown(5) == 0);
    CHECK(a.call("cellSysutilAvc2ShowWindow", 9) == kNotFound);
    CHECK(a.call("cellSysutilAvc2HideWindow", 9) == kNotFound);
    CHECK(window_shown(9) == 0);

    // Position and size: nothing written until the game sets them, then what it set (floats in f1..).
    std::memset(base + kOut, 0xAA, 16);
    REQUIRE(a.call("cellSysutilAvc2GetWindowPosition", 5, kOut, kOut + 4, kOut + 8) == 0);
    REQUIRE(a.call("cellSysutilAvc2GetWindowSize", 5, kOut, kOut + 4) == 0);
    CHECK(ps_read32(base, kOut) == 0xAAAAAAAAu);
    CHECK(ps_read32(base, kOut + 8) == 0xAAAAAAAAu);
    REQUIRE(position(5, 0.25, -0.5, 1.0) == 0);
    REQUIRE(a.call("cellSysutilAvc2GetWindowPosition", 5, kOut, kOut + 4, kOut + 8) == 0);
    CHECK(float_at(base, kOut) == 0.25f);
    CHECK(float_at(base, kOut + 4) == -0.5f);
    CHECK(float_at(base, kOut + 8) == 1.0f);
    REQUIRE(size(5, 160.0, 120.0) == 0);
    REQUIRE(a.call("cellSysutilAvc2GetWindowSize", 5, kOut, kOut + 4) == 0);
    CHECK(float_at(base, kOut) == 160.0f);
    CHECK(float_at(base, kOut + 4) == 120.0f);
    CHECK(position(9, 0, 0, 0) == kNotFound);
    CHECK(size(9, 1, 1) == kNotFound);
    CHECK(a.call("cellSysutilAvc2GetWindowPosition", 5, kOut, 0, kOut + 8) == kInval);
    CHECK(a.call("cellSysutilAvc2GetWindowSize", 5, kUnmapped, kOut) == kInval); // RPCS3 faults
    std::memset(base + kOut, 0xAA, 16);
    REQUIRE(a.call("cellSysutilAvc2GetWindowSize", 0xFFF0, kOut, kOut + 4) == 0); // a window never sized
    CHECK(ps_read32(base, kOut) == 0xAAAAAAAAu);

    // The screen: hidden until shown.
    CHECK(screen_shown() == 0);
    REQUIRE(a.call("cellSysutilAvc2ShowScreen") == 0);
    CHECK(screen_shown() == 1);
    REQUIRE(a.call("cellSysutilAvc2HideScreen") == 0);
    CHECK(screen_shown() == 0);

    // The resolution: QQVGA or QVGA, up to the load's.
    CHECK(a.call("cellSysutilAvc2ChangeVideoResolution", 2) == kInval);
    CHECK(a.call("cellSysutilAvc2ChangeVideoResolution", 0) == kInval);
    CHECK(a.call("cellSysutilAvc2ChangeVideoResolution", 1) == 0);

    // A window destroyed makes room for another.
    REQUIRE(a.call("cellSysutilAvc2ShowWindow", 5) == 0);
    REQUIRE(a.call("cellSysutilAvc2DestroyWindow", 5) == 0);
    CHECK(a.call("cellSysutilAvc2DestroyWindow", 5) == kNotFound);
    CHECK(window_shown(5) == 0);
    REQUIRE(a.call("cellSysutilAvc2CreateWindow", 7) == 0);

    // The video chat unloaded: its windows and screen go; the voice chat stays.
    REQUIRE(a.call("cellSysutilAvc2ShowWindow", 7) == 0);
    REQUIRE(a.call("cellSysutilAvc2ShowScreen") == 0);
    REQUIRE(a.call("cellSysutilAvc2Unload2", 0x10) == 0);
    CHECK(window_shown(7) == 0);
    CHECK(screen_shown() == 0);
    CHECK(a.call("cellSysutilAvc2ShowWindow", 7) == kNotInit);
    CHECK(a.call("cellSysutilAvc2DestroyWindow", 7) == kNotInit);

    // Loaded again for one QVGA window, which may change down and up again; UnloadAsync2 ends it at the call.
    put_video_param(base, kParam, 1, 2, 1);
    REQUIRE(load(0) == 0);
    CHECK(a.call("cellSysutilAvc2ChangeVideoResolution", 1) == 0);
    CHECK(a.call("cellSysutilAvc2ChangeVideoResolution", 2) == 0);
    REQUIRE(a.call("cellSysutilAvc2CreateWindow", 1) == 0);
    CHECK(a.call("cellSysutilAvc2CreateWindow", 2) == kTooMany);
    REQUIRE(a.call("cellSysutilAvc2UnloadAsync2", 0x10) == 0);
    CHECK(a.call("cellSysutilAvc2CreateWindow", 2) == kNotInit);
    CHECK(take(a, base, 3) == std::vector<Entry>{{3, 2, 0, 0}}); // the voice chat's callback stays

    // Unload ends both; with only a video chat loaded it unloads that.
    REQUIRE(load(0) == 0);
    REQUIRE(a.call("cellSysutilAvc2CreateWindow", 1) == 0);
    REQUIRE(a.call("cellSysutilAvc2Unload") == 0);
    CHECK(a.call("cellSysutilAvc2ShowWindow", 1) == kNotInit);
    REQUIRE(load(0) == 0);
    REQUIRE(a.call("cellSysutilAvc2Unload") == 0);
    CHECK(a.call("cellSysutilAvc2ShowScreen") == kNotInit);
    CHECK(a.call("cellSysutilAvc2Unload") == kNotInit);
}

TEST_CASE("cellSysutilAvc2: streaming in a chat with nobody else, its target and priority, voice detection (D-675)",
    "[hle][avc][avc2]")
{
    InterpretMisses interpret;
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* base = f.base();
    write_callback(base);
    constexpr u32 kParam = kData + 0x100, kRoom = kData + 0x200, kTarget = kData + 0x300, kMembers = kData + 0x340,
                  kAddrs = kData + 0x360, kPorts = kData + 0x380, kArg = 0x77;
    ps_write64(base, kRoom, 0x0001000200030004ull);
    const auto load = [&](u32 callback) { return a.call("cellSysutilAvc2Load", 1, 0, callback, kArg, kParam); };
    const auto target = [&](u32 mode, u32 list = 0, u8 n = 0, u32 addrs = 0, u32 ports = 0) {
        std::memset(base + kTarget, 0, 20);
        ps_write32(base, kTarget, mode);
        if (mode == 0x200) {
            base[kTarget + 4] = 3; // the team
        } else if (mode == 0x300) {
            ps_write32(base, kTarget + 4, list);
            base[kTarget + 8] = n;
        } else if (mode == 0x1000) {
            ps_write32(base, kTarget + 4, list);
            ps_write32(base, kTarget + 8, addrs);
            ps_write32(base, kTarget + 12, ports);
            ps_write16(base, kTarget + 16, 1);
            base[kTarget + 18] = n;
        }
        return a.call("cellSysutilAvc2SetStreamingTarget", kTarget);
    };
    for (u32 i = 0; i < 4; i++) {
        ps_write16(base, kMembers + 2 * i, static_cast<u16>(2 + i));
        ps_write32(base, kAddrs + 4 * i, 0xC0A80002 + i);
        ps_write16(base, kPorts + 2 * i, static_cast<u16>(3658 + i));
    }

    // Nothing before a load; a bad target is refused first; leaving no chat changes nothing.
    CHECK(a.call("cellSysutilAvc2StartStreaming") == kNotInit);
    CHECK(a.call("cellSysutilAvc2StopStreaming") == kNotInit);
    CHECK(a.call("cellSysutilAvc2SetStreamPriority", 1) == kNotInit);
    CHECK(target(0x100) == kNotInit);
    CHECK(a.call("cellSysutilAvc2SetStreamingTarget", 0) == kInval);
    CHECK(a.call("cellSysutilAvc2StartVoiceDetection") == kNotInit);
    CHECK(a.call("cellSysutilAvc2LeaveChat") == 0);

    // Loaded: streaming needs a chat.
    put_voice_param(base, kParam, 140, 8, 4);
    REQUIRE(load(kOpd) == 0);
    CHECK(a.call("cellSysutilAvc2StartStreaming") == kInvalStatus);
    CHECK(a.call("cellSysutilAvc2StopStreaming") == kInvalStatus);

    // The join pspocket answers as succeeded leaves a chat to stream in, with nobody else in it.
    REQUIRE(a.call("cellSysutilAvc2JoinChatRequest", kRoom) == 0);
    CHECK(a.call("cellSysutilAvc2StartStreaming") == 0);
    CHECK(a.call("cellSysutilAvc2StartStreaming") == 0);
    CHECK(a.call("cellSysutilAvc2StopStreaming") == 0);
    CHECK(a.call("cellSysutilAvc2StopStreaming") == 0);
    REQUIRE(a.call("cellSysutilAvc2StartStreaming") == 0);
    // LeaveChat leaves at once, with no event (as the synchronous JoinChat joins).
    REQUIRE(a.call("cellSysutilAvc2LeaveChat") == 0);
    CHECK(a.call("cellSysutilAvc2StartStreaming") == kInvalStatus);
    CHECK(take(a, base, 3) == std::vector<Entry>{{5, 0, kArg, 0}});
    // And LeaveChatRequest at the call, its event at the next check.
    REQUIRE(a.call("cellSysutilAvc2JoinChat", kRoom, 0, 0) == 0);
    CHECK(a.call("cellSysutilAvc2StartStreaming") == 0);
    REQUIRE(a.call("cellSysutilAvc2LeaveChatRequest") == 0);
    CHECK(a.call("cellSysutilAvc2StopStreaming") == kInvalStatus);
    CHECK(take(a, base, 3) == std::vector<Entry>{{7, 0, kArg, 0}});

    // The target: the room, a team, members, or members at addresses; the lists one member at least, guest memory.
    CHECK(target(0x100) == 0);
    CHECK(target(0x200) == 0);
    CHECK(target(0x300, kMembers, 3) == 0);
    CHECK(target(0x300, kMembers, 0) == kInval);
    CHECK(target(0x300, 0, 2) == kInval);
    CHECK(target(0x300, kUnmapped, 2) == kInval); // RPCS3 reads nothing
    CHECK(target(0x1000, kMembers, 4, kAddrs, kPorts) == 0);
    CHECK(target(0x1000, kMembers, 2, kAddrs, 0) == kInval);
    CHECK(target(0x1000, kMembers, 0, kAddrs, kPorts) == kInval);
    CHECK(target(0x400) == kInval);
    CHECK(target(0) == kInval);
    CHECK(a.call("cellSysutilAvc2SetStreamingTarget", kUnmapped) == kInval);

    // The priority, kept as given; voice detection switched, nothing ever detected (no microphone).
    CHECK(a.call("cellSysutilAvc2SetStreamPriority", 0x102) == 0);
    REQUIRE(a.call("cellSysutilAvc2StartVoiceDetection") == 0);
    CHECK(a.call("cellSysutilAvc2StartVoiceDetection") == 0);
    REQUIRE(a.call("cellSysutilAvc2StopVoiceDetection") == 0);
    REQUIRE(a.call("cellSysutilAvc2StartVoiceDetection") == 0);
    CHECK(take(a, base, 3).empty());

    // Unloading the voice chat ends the chat: loaded again, there is none.
    REQUIRE(a.call("cellSysutilAvc2JoinChat", kRoom, 0, 0) == 0);
    REQUIRE(a.call("cellSysutilAvc2StartStreaming") == 0);
    REQUIRE(a.call("cellSysutilAvc2Unload2", 1) == 0);
    CHECK(a.call("cellSysutilAvc2StopStreaming") == kNotInit);
    CHECK(a.call("cellSysutilAvc2StopVoiceDetection") == kNotInit);
    REQUIRE(load(kOpd) == 0);
    CHECK(a.call("cellSysutilAvc2StartStreaming") == kInvalStatus);

    // Voice detection is the voice chat's; streaming is the chat's, whichever chat is loaded.
    REQUIRE(a.call("cellSysutilAvc2Unload") == 0);
    put_video_param(base, kParam, 1, 1, 2);
    REQUIRE(load(0) == 0);
    CHECK(a.call("cellSysutilAvc2StartVoiceDetection") == kNotInit);
    REQUIRE(a.call("cellSysutilAvc2JoinChat", kRoom, 0, 0) == 0);
    CHECK(a.call("cellSysutilAvc2StartStreaming") == 0);
    CHECK(target(0x100) == 0);
    REQUIRE(a.call("cellSysutilAvc2UnloadAsync") == 0);
    CHECK(a.call("cellSysutilAvc2StartStreaming") == kNotInit);
}
