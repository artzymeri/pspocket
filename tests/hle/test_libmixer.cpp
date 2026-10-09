// libmixer (D-563): pspocket's surround mixer over pspocket's cellAudio, made from RPCS3's libmixer.cpp (no Sony module
// to compare with). Synthetic: a game's notify callback in guest code that feeds the channel strips through the AAN and
// a speaker bus every block, a sample player, and what reaches the speakers and the mixer's audio port. No game data.

#include "hle_test_util.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <thread>
#include <vector>

#include "pspocket/hle/audio.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace std::chrono_literals;
namespace as = pspocket::recomp::ppu::as;

namespace {

constexpr u32 kConfig = kData + 0x100, kOut = kData + 0x140, kStrip = kData + 0x180, kTag = kData + 0x1C0,
              kStamp = kData + 0x1C8, kInfo = kData + 0x200, kWave = kData + 0x240, kCommon = kData + 0x260,
              kPlayerConfig = kData + 0x270, kAudioConfig = kData + 0x280, kRecords = kData + 0x400,
              kCode = kData + 0x1000, kCallback = kData + 0x1F00, kMono = kData + 0x2000, kStereo = kData + 0x2400,
              kSur6 = kData + 0x2C00, kSur8 = kData + 0x4400, kBus = kData + 0x6400, kPcm = kData + 0x6800;
const u64 kNotInitialized = r3_of_code(0x80310002), kInvalid = r3_of_code(0x80310003),
          kAlreadyExist = r3_of_code(0x80310006), kNotExist = r3_of_code(0x80310008);
constexpr u32 kAan = 0x11111111;
constexpr u32 kStatusClose = 0x1010, kStatusReady = 1, kStatusRun = 2;

// The game's notify callback, guest code: records (arg, counter, samples) at kRecords (a count, then 16 bytes each from
// +16, the last 64 kept), then, when `feed`, adds the block's data: the mono, stereo, 7.1 and 5.1 strips (index 0 of
// each) through cellAANAddData and bus 3 through cellSurMixerSurBusAddData. Returns 0.
void write_callback(Fixture& f, bool feed)
{
    u8* b = f.base();
    const u32 aan = f.hle->link_function("libmixer", recomp::nid_of("cellAANAddData"), "cellAANAddData");
    const u32 bus = f.hle->link_function("libmixer", recomp::nid_of("cellSurMixerSurBusAddData"),
        "cellSurMixerSurBusAddData");
    REQUIRE(aan);
    REQUIRE(bus);
    std::vector<u32> c;
    const auto load = [&](u32 reg, u32 value) {
        c.push_back(as::lis(reg, static_cast<s32>(value >> 16)));
        c.push_back(as::ori(reg, reg, value & 0xFFFF));
    };
    const auto call = [&](u32 opd, std::initializer_list<u32> args) {
        u32 reg = 3;
        for (const u32 a : args) load(reg++, a);
        load(12, opd);
        c.push_back(as::lwz(0, 0, 12));
        c.push_back(as::lwz(2, 4, 12));
        c.push_back(as::mtctr(0));
        c.push_back(as::bctrl());
    };
    c.push_back(as::mflr(0));
    c.push_back(as::std_(0, 16, 1));
    c.push_back(as::stdu(1, -0x80, 1));
    load(11, kRecords);
    c.push_back(as::lwz(12, 0, 11));
    c.push_back(as::addi(10, 12, 1));
    c.push_back(as::stw(10, 0, 11));
    c.push_back(as::andi_(12, 12, 63));
    c.push_back(as::mulli(12, 12, 16));
    c.push_back(as::add(12, 11, 12));
    c.push_back(as::stw(3, 16, 12));
    c.push_back(as::stw(4, 20, 12));
    c.push_back(as::stw(5, 24, 12));
    if (feed) {
        call(aan, {kAan, 0x10000, 0, kMono, 256});
        call(aan, {kAan, 0x20000, 0, kStereo, 256});
        call(aan, {kAan, 0x40000, 0, kSur8, 256});
        call(aan, {kAan, 0x30000, 0, kSur6, 256});
        call(bus, {3, 0, kBus, 256});
    }
    c.push_back(as::addi(1, 1, 0x80));
    c.push_back(as::ld(0, 16, 1));
    c.push_back(as::mtlr(0));
    c.push_back(as::li(3, 0));
    c.push_back(as::blr());
    REQUIRE(4 * c.size() <= kCallback - kCode);
    for (u32 i = 0; i < c.size(); i++) ps_write32(b, kCode + 4 * i, c[i]);
    ps_write32(b, kCallback, kCode);
    ps_write32(b, kCallback + 4, 0);
    ps_write32(b, kRecords, 0);
}

struct Record {
    u32 arg, counter, samples;
};

u32 records(u8* b) { return ps_read32(b, kRecords); }
Record record(u8* b, u32 i)
{
    const u32 at = kRecords + 16 + 16 * (i & 63);
    return {ps_read32(b, at), ps_read32(b, at + 4), ps_read32(b, at + 8)};
}

bool wait_for(const std::function<bool()>& done)
{
    for (int i = 0; i < 400; i++) {
        if (done()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return done();
}

void write_config(u8* b, s32 priority, std::array<u32, 4> strips)
{
    ps_write32(b, kConfig, static_cast<u32>(priority));
    for (u32 i = 0; i < 4; i++) ps_write32(b, kConfig + 4 + 4 * i, strips[i]);
}

u32 port_status(GuestThread& g, u32 port)
{
    REQUIRE(g.call("cellAudioGetPortConfig", port, kAudioConfig) == 0);
    return ps_read32(g.f.base(), kAudioConfig + 4);
}

// cellSurMixerChStripSetParameter(type, index, {param, 0, dBSwitch, floatVal, intVal}).
u64 strip_param(GuestThread& g, u32 type, u32 index, u32 param, bool db, f32 fv, s32 iv)
{
    u8* b = g.f.base();
    ps_write32(b, kStrip, param);
    ps_write32(b, kStrip + 4, 0);
    ps_write32(b, kStrip + 8, db ? 1 : 0);
    ps_writef32(b, kStrip + 12, fv);
    ps_write32(b, kStrip + 16, static_cast<u32>(iv));
    return g.call("cellSurMixerChStripSetParameter", type, index, kStrip);
}

u64 set_parameter(GuestThread& g, u32 param, f32 value)
{
    g.ctx.fpr[1] = static_cast<double>(value);
    return g.call("cellSurMixerSetParameter", param);
}

f32 f32_result(GuestThread& g) { return static_cast<f32>(g.ctx.fpr[1]); }

} // namespace

TEST_CASE("libmixer: create, the AAN's handle and port numbers, the notify callback, start, pause and finalize",
    "[hle][libmixer]")
{
    Fixture f;
    REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
    GuestThread g(f, 0, "game");
    ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
    u8* b = f.base();
    write_callback(f, false);
    write_config(b, 100, {1, 1, 1, 1});

    CHECK(g.call("cellSurMixerCreate", kConfig) == kNotInitialized); // cellAudioInit first
    CHECK(g.call("cellSurMixerStart") == kNotInitialized);
    CHECK(g.call("cellSurMixerPause", 1) == kNotInitialized);
    CHECK(g.call("cellSurMixerFinalize") == kNotInitialized);
    CHECK(set_parameter(g, 8, 0.5f) == kNotInitialized);
    REQUIRE(g.call("cellAudioInit") == 0);
    REQUIRE(g.call("cellSurMixerCreate", kConfig) == 0);
    CHECK(g.call("cellSurMixerCreate", kConfig) == kAlreadyExist);
    // Its audio port: 8 channels, 16 blocks, open.
    CHECK(port_status(g, 0) == kStatusReady);
    CHECK(ps_read64(b, kAudioConfig + 8) == 8);
    CHECK(ps_read64(b, kAudioConfig + 16) == 16);

    REQUIRE(g.call("cellSurMixerGetAANHandle", kOut) == 0);
    CHECK(ps_read32(b, kOut) == kAan);
    REQUIRE(g.call("cellSurMixerChStripGetAANPortNo", kOut, 4, 2) == 0);
    CHECK(ps_read32(b, kOut) == 0x40002);
    CHECK(g.call("cellSurMixerGetAANHandle", 0) == kInvalid);

    CHECK(g.call("cellSurMixerSetNotifyCallback", 0, 0) == kInvalid);
    REQUIRE(g.call("cellSurMixerSetNotifyCallback", kCallback, 0xC0FFEE) == 0);
    CHECK(g.call("cellSurMixerSetNotifyCallback", kCallback, 0) == kAlreadyExist);
    CHECK(g.call("cellSurMixerRemoveNotifyCallback", kCallback + 8) == kNotExist);
    REQUIRE(g.call("cellSurMixerRemoveNotifyCallback", kCallback) == 0);
    CHECK(g.call("cellSurMixerRemoveNotifyCallback", kCallback) == kNotExist);
    REQUIRE(g.call("cellSurMixerSetNotifyCallback", kCallback, 0xC0FFEE) == 0);

    // Nothing is mixed before the start.
    std::this_thread::sleep_for(30ms);
    CHECK(records(b) == 0);
    REQUIRE(g.call("cellSurMixerGetCurrentBlockTag", kTag) == 0);
    CHECK(ps_read64(b, kTag) == 0);

    // A block each period: the callback with its argument, the block's tag (from 0) and 256 samples.
    REQUIRE(g.call("cellSurMixerStart") == 0);
    CHECK(g.call("cellSurMixerStart") == 0);
    CHECK(port_status(g, 0) == kStatusRun);
    REQUIRE(wait_for([&] { return records(b) >= 6; }));
    const u32 n = std::min(records(b), 64u);
    for (u32 i = 0; i < n; i++) {
        INFO("block " << i);
        CHECK(record(b, i).arg == 0xC0FFEE);
        CHECK(record(b, i).counter == i);
        CHECK(record(b, i).samples == 256);
    }
    REQUIRE(g.call("cellSurMixerGetCurrentBlockTag", kTag) == 0);
    CHECK(ps_read64(b, kTag) >= 6);
    // Timestamps: a period (256 samples at 48 kHz) a block from the start.
    REQUIRE(g.call("cellSurMixerGetTimestamp", 0, kStamp) == 0);
    const u64 t0 = ps_read64(b, kStamp);
    CHECK(t0 > 0);
    REQUIRE(g.call("cellSurMixerGetTimestamp", 3, kStamp) == 0);
    CHECK(ps_read64(b, kStamp) == t0 + 16000);
    REQUIRE(g.call("cellSurMixerGetTimestamp", 1, kStamp) == 0);
    CHECK(ps_read64(b, kStamp) == t0 + 5333);

    // Paused: the port stops and no block is mixed; playing again, they go on.
    CHECK(g.call("cellSurMixerPause", 3) == kInvalid);
    REQUIRE(g.call("cellSurMixerPause", 1) == 0);
    CHECK(port_status(g, 0) == kStatusReady);
    std::this_thread::sleep_for(10ms); // a block being mixed when the pause came
    const u32 paused = records(b);
    std::this_thread::sleep_for(30ms);
    CHECK(records(b) == paused);
    REQUIRE(g.call("cellSurMixerPause", 0) == 0);
    CHECK(port_status(g, 0) == kStatusRun);
    CHECK(wait_for([&] { return records(b) >= paused + 3; }));
    CHECK(record(b, paused).counter == paused); // the tags go on from where they stopped

    // Finalized: the mixer's thread ends, its port closes; a new mixer can be made.
    REQUIRE(g.call("cellSurMixerFinalize") == 0);
    const u32 done = records(b);
    std::this_thread::sleep_for(20ms);
    CHECK(records(b) == done);
    CHECK(port_status(g, 0) == kStatusClose);
    CHECK(g.call("cellSurMixerFinalize") == kNotInitialized);
    REQUIRE(g.call("cellSurMixerCreate", kConfig) == 0);
    CHECK(port_status(g, 0) == kStatusReady);
    REQUIRE(g.call("cellSurMixerFinalize") == 0);
    REQUIRE(g.call("cellAudioQuit") == 0);
}

TEST_CASE("libmixer: a block's mix: the strips with their levels and mutes, a speaker bus, a sample player, the total "
          "level, in the mixer's port and at the speakers",
    "[hle][libmixer]")
{
    Fixture f;
    REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
    GuestThread g(f, 0, "game");
    ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
    u8* b = f.base();
    write_callback(f, true);
    write_config(b, 100, {1, 1, 1, 1});

    // What the callback adds each block: mono 0.5; stereo 0.25, -0.125; 7.1 (c + 1) / 16 on channel c; 5.1 1.0 on
    // every channel (its strip muted); bus 3 0.2. The player: a mono wave of 512 samples at half scale, looped.
    for (u32 i = 0; i < 256; i++) {
        ps_writef32(b, kMono + 4 * i, 0.5f);
        ps_writef32(b, kStereo + 8 * i, 0.25f);
        ps_writef32(b, kStereo + 8 * i + 4, -0.125f);
        for (u32 c = 0; c < 8; c++) ps_writef32(b, kSur8 + 32 * i + 4 * c, static_cast<f32>(c + 1) * 0.0625f);
        for (u32 c = 0; c < 6; c++) ps_writef32(b, kSur6 + 24 * i + 4 * c, 1.0f);
        ps_writef32(b, kBus + 4 * i, 0.2f);
    }
    for (u32 i = 0; i < 512; i++) { // little-endian 16-bit samples
        ps_write8(b, kPcm + 2 * i, 0x00);
        ps_write8(b, kPcm + 2 * i + 1, 0x40);
    }

    struct Heard {
        std::mutex mutex;
        std::vector<std::array<f32, 2>> blocks; // the first sample of each period (all are checked equal)
        u32 uneven = 0;
    } heard;
    hle::set_audio_sink(*f.hle, [&](const f32* samples, u32 frames) {
        bool even = true;
        for (u32 i = 1; i < frames; i++) even = even && samples[2 * i] == samples[0] && samples[2 * i + 1] == samples[1];
        std::lock_guard lock(heard.mutex);
        heard.blocks.push_back({samples[0], samples[1]});
        heard.uneven += !even;
    });
    const auto heard_since = [&](usize from, std::array<f32, 2> want) {
        std::lock_guard lock(heard.mutex);
        u32 n = 0;
        for (usize i = from; i < heard.blocks.size(); i++)
            n += std::fabs(heard.blocks[i][0] - want[0]) < 1e-6f && std::fabs(heard.blocks[i][1] - want[1]) < 1e-6f;
        return n;
    };
    const auto heard_count = [&] {
        std::lock_guard lock(heard.mutex);
        return heard.blocks.size();
    };

    REQUIRE(g.call("cellAudioInit") == 0);
    REQUIRE(g.call("cellSurMixerCreate", kConfig) == 0);

    // Strips: the mono one at level 0.5, the 7.1 one at expression 0.5 (in dB), the 5.1 one muted.
    CHECK(strip_param(g, 1, 0, 0, false, 0.5f, 0) == 0);
    CHECK(strip_param(g, 4, 0, 1, true, 20.0f * std::log10(0.5f), 0) == 0);
    CHECK(strip_param(g, 3, 0, 4, false, 0.0f, 1) == 0);
    CHECK(strip_param(g, 1, 0, 6, false, 0.0f, 0) == 0);        // a position: taken, not mixed
    CHECK(strip_param(g, 4, 0, 6, false, 0.0f, 0) == kInvalid); // no positions for 7.1
    CHECK(strip_param(g, 1, 1, 0, false, 1.0f, 0) == kInvalid); // one mono strip only
    CHECK(strip_param(g, 5, 0, 0, false, 1.0f, 0) == kInvalid);
    // The total level 0.5; an unknown parameter.
    CHECK(set_parameter(g, 8, 0.5f) == 0);
    CHECK(set_parameter(g, 99, 1.0f) == kInvalid);

    // The player: one channel, the wave looped from its start, connected to the mixer, played at level 0.5.
    ps_write32(b, kPlayerConfig, 3);
    ps_write32(b, kPlayerConfig + 4, 0);
    CHECK(g.call("cellSSPlayerCreate", kOut, kPlayerConfig) == kInvalid); // 1 or 2 channels
    ps_write32(b, kPlayerConfig, 1);
    REQUIRE(g.call("cellSSPlayerCreate", kOut, kPlayerConfig) == 0);
    const u32 player = ps_read32(b, kOut);
    CHECK(player == 0);
    ps_write32(b, kWave, kPcm);
    ps_write32(b, kWave + 4, 0);
    ps_write32(b, kWave + 8, 512);
    ps_write32(b, kWave + 12, 1);
    ps_write32(b, kWave + 16, 1);
    ps_write32(b, kCommon, 16); // CELL_SSPLAYER_LOOP_ON
    ps_write32(b, kCommon + 4, 0);
    REQUIRE(g.call("cellSSPlayerSetWave", player, kWave, kCommon) == 0);
    CHECK(g.call("cellSSPlayerSetWave", player + 1, kWave, kCommon) == kInvalid);
    CHECK(g.call("cellAANConnect", kAan, 0, player + 1, 0) == kInvalid);
    REQUIRE(g.call("cellAANConnect", kAan, 0, player, 0) == 0);
    CHECK(g.call("cellSSPlayerGetState", player) == 0x00); // OFF
    ps_writef32(b, kInfo, 0.5f);
    ps_writef32(b, kInfo + 4, 1.0f);
    for (u32 i = 2; i < 5; i++) ps_writef32(b, kInfo + 4 * i, 0.0f);
    REQUIRE(g.call("cellSSPlayerPlay", player, kInfo) == 0);
    CHECK(g.call("cellSSPlayerGetState", player) == 0x20); // ON
    CHECK(g.call("cellSSPlayerGetState", player + 1) == ~u64{0}); // ERROR (-1)

    REQUIRE(g.call("cellSurMixerSetNotifyCallback", kCallback, 0) == 0);
    REQUIRE(g.call("cellSurMixerStart") == 0);

    // The expected block, worked out in the mixer's order: the strips (times their level and expression), the bus,
    // the player, then the total level.
    std::array<f32, 8> want{};
    want[0] += 0.5f * 0.5f;
    want[1] += 0.5f * 0.5f;
    want[0] += 0.25f;
    want[1] += -0.125f;
    const f32 expression = std::pow(10.0f, 20.0f * std::log10(0.5f) / 20.0f);
    for (u32 c = 0; c < 8; c++) want[c] += static_cast<f32>(c + 1) * 0.0625f * (1.0f * expression);
    want[3] += 0.2f;
    const f32 voice = static_cast<f32>(0x4000) / 32768.0f * 0.5f;
    want[0] += voice;
    want[1] += voice;
    for (f32& w : want) w *= 0.5f;

    // At the speakers, the port's front left and right; every sample of a block alike.
    CHECK(wait_for([&] { return heard_since(0, {want[0], want[1]}) >= 4; }));
    // In the port's ring, all eight channels of a block the mixer wrote (two periods ahead of the read index).
    const u32 ring = [&] {
        REQUIRE(g.call("cellAudioGetPortConfig", 0, kAudioConfig) == 0);
        return ps_read32(b, kAudioConfig + 0x1C);
    }();
    const auto ring_has_block = [&] {
        for (u32 block = 0; block < 16; block++) {
            const u32 at = ring + block * 8 * 256 * 4;
            bool all = true;
            for (u32 i = 0; i < 256 && all; i++)
                for (u32 c = 0; c < 8 && all; c++) all = std::fabs(ps_readf32(b, at + 32 * i + 4 * c) - want[c]) < 1e-6f;
            if (all) return true;
        }
        return false;
    };
    CHECK(wait_for(ring_has_block));

    // The total mute silences it; off again, it is back.
    usize from = heard_count();
    CHECK(set_parameter(g, 12, 1.0f) == 0);
    CHECK(wait_for([&] { return heard_since(from, {0.0f, 0.0f}) >= 3; }));
    from = heard_count();
    CHECK(set_parameter(g, 12, 0.0f) == 0);
    CHECK(wait_for([&] { return heard_since(from, {want[0], want[1]}) >= 3; }));

    // The player stopped: its part leaves the mix.
    REQUIRE(g.call("cellSSPlayerStop", player, 0) == 0);
    CHECK(g.call("cellSSPlayerGetState", player) == 0x00);
    from = heard_count();
    CHECK(wait_for([&] { return heard_since(from, {want[0] - voice * 0.5f, want[1] - voice * 0.5f}) >= 3; }));
    REQUIRE(g.call("cellSSPlayerRemove", player) == 0);
    CHECK(g.call("cellSSPlayerGetState", player) == ~u64{0});
    CHECK(g.call("cellSSPlayerRemove", player) == kInvalid);

    REQUIRE(g.call("cellSurMixerFinalize") == 0);
    hle::set_audio_sink(*f.hle, {});
    REQUIRE(g.call("cellAudioQuit") == 0);
    std::lock_guard lock(heard.mutex);
    CHECK(heard.uneven == 0);
}

TEST_CASE("libmixer: a sample player's speed, loop modes and stereo waves", "[hle][libmixer]")
{
    Fixture f;
    REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
    GuestThread g(f, 0, "game");
    ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
    u8* b = f.base();
    write_config(b, 100, {0, 0, 0, 0});

    std::mutex mutex;
    std::vector<std::array<f32, 2 * hle::kAudioBlockFrames>> blocks;
    hle::set_audio_sink(*f.hle, [&](const f32* samples, u32 frames) {
        std::lock_guard lock(mutex);
        auto& block = blocks.emplace_back();
        std::copy(samples, samples + 2 * std::min(frames, hle::kAudioBlockFrames), block.begin());
    });
    // The first block of the player's sound at the speakers (a block whose first sample is not silent), from `from`.
    const auto first_sound = [&](usize from, std::array<f32, 2 * hle::kAudioBlockFrames>& out) {
        return wait_for([&] {
            std::lock_guard lock(mutex);
            for (usize i = from; i < blocks.size(); i++) {
                if (blocks[i][0] != 0.0f || blocks[i][1] != 0.0f) {
                    out = blocks[i];
                    return true;
                }
            }
            return false;
        });
    };
    const auto count = [&] {
        std::lock_guard lock(mutex);
        return blocks.size();
    };

    REQUIRE(g.call("cellAudioInit") == 0);
    REQUIRE(g.call("cellSurMixerCreate", kConfig) == 0);
    REQUIRE(g.call("cellSurMixerStart") == 0);

    // A stereo wave of 100 frames: left i, right -i (in 1/32768ths), played once at double speed from its 11th frame
    // until it ends, then silence.
    for (u32 i = 0; i < 100; i++) {
        const u16 left = static_cast<u16>(i), right = static_cast<u16>(-static_cast<s32>(i));
        ps_write8(b, kPcm + 4 * i, static_cast<u8>(left));
        ps_write8(b, kPcm + 4 * i + 1, static_cast<u8>(left >> 8));
        ps_write8(b, kPcm + 4 * i + 2, static_cast<u8>(right));
        ps_write8(b, kPcm + 4 * i + 3, static_cast<u8>(right >> 8));
    }
    ps_write32(b, kPlayerConfig, 2);
    ps_write32(b, kPlayerConfig + 4, 0);
    REQUIRE(g.call("cellSSPlayerCreate", kOut, kPlayerConfig) == 0);
    const u32 player = ps_read32(b, kOut);
    ps_write32(b, kWave, kPcm);
    ps_write32(b, kWave + 8, 100);
    ps_write32(b, kWave + 12, 1);
    ps_write32(b, kWave + 16, 11);
    REQUIRE(g.call("cellSSPlayerSetWave", player, kWave, 0) == 0); // no common parameters: one shot
    REQUIRE(g.call("cellAANConnect", kAan, 0, player, 0) == 0);
    ps_writef32(b, kInfo, 1.0f);
    ps_writef32(b, kInfo + 4, 2.0f);
    usize from = count();
    REQUIRE(g.call("cellSSPlayerPlay", player, kInfo) == 0);
    std::array<f32, 2 * hle::kAudioBlockFrames> block{};
    REQUIRE(first_sound(from, block));
    // RPCS3's rough speed change: at double speed two frames go into each output sample, added, so output sample k
    // holds frames 10 + 2k and 11 + 2k, until the wave's end (frames 98 and 99 in sample 44).
    CHECK(block[0] == 21.0f / 32768.0f);
    CHECK(block[1] == -21.0f / 32768.0f);
    CHECK(block[2] == 25.0f / 32768.0f);
    CHECK(block[3] == -25.0f / 32768.0f);
    CHECK(block[2 * 44] == 197.0f / 32768.0f);
    CHECK(block[2 * 44 + 1] == -197.0f / 32768.0f);
    CHECK(block[2 * 45] == 0.0f); // the wave ended: the player stopped
    CHECK(wait_for([&] { return g.call("cellSSPlayerGetState", player) == 0x00; }));

    // Looped at normal speed from frame 0, the loop from frame 90: 0..99, then 90..99 again and again.
    ps_write32(b, kWave + 12, 91);
    ps_write32(b, kWave + 16, 1);
    ps_write32(b, kCommon, 16);
    REQUIRE(g.call("cellSSPlayerSetWave", player, kWave, kCommon) == 0);
    ps_writef32(b, kInfo + 4, 1.0f);
    from = count();
    REQUIRE(g.call("cellSSPlayerPlay", player, kInfo) == 0);
    REQUIRE(first_sound(from, block)); // the first block starts with frame 0, silent: a later one is found
    const auto frame_of = [](f32 v) { return static_cast<s32>(std::lround(v * 32768.0f)); };
    s32 prev = frame_of(block[0]);
    bool looped = false, steps = true;
    for (u32 i = 1; i < hle::kAudioBlockFrames; i++) {
        const s32 cur = frame_of(block[2 * i]);
        if (cur == 90 && prev == 99) looped = true;
        else steps = steps && cur == prev + 1;
        steps = steps && frame_of(block[2 * i + 1]) == -cur;
        prev = cur;
    }
    CHECK(looped);
    CHECK(steps);
    CHECK(g.call("cellSSPlayerGetState", player) == 0x20);

    // A start past the wave's end: stopped rather than read past it.
    ps_write32(b, kWave + 16, 200);
    REQUIRE(g.call("cellSSPlayerSetWave", player, kWave, kCommon) == 0);
    CHECK(wait_for([&] { return g.call("cellSSPlayerGetState", player) == 0x00; }));
    // A wave outside guest memory is refused.
    ps_write32(b, kWave, 0xE0000000);
    CHECK(g.call("cellSSPlayerSetWave", player, kWave, kCommon) == kInvalid);

    REQUIRE(g.call("cellSurMixerFinalize") == 0);
    hle::set_audio_sink(*f.hle, {});
    REQUIRE(g.call("cellAudioQuit") == 0);
}

TEST_CASE("libmixer: the utilities: levels from decibels, note ratios", "[hle][libmixer]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    const auto db = [&](f32 v) {
        g.ctx.fpr[1] = static_cast<double>(v);
        g.call("cellSurMixerUtilGetLevelFromDB");
        return f32_result(g);
    };
    CHECK(db(0.0f) == 1.0f);
    CHECK(db(-20.0f) == Catch::Approx(0.1f).epsilon(1e-6));
    CHECK(db(20.0f * std::log10(2.0f)) == Catch::Approx(2.0f).epsilon(1e-6));
    g.call("cellSurMixerUtilGetLevelFromDBIndex", static_cast<u32>(-20));
    CHECK(f32_result(g) == Catch::Approx(0.1f).epsilon(1e-6));
    g.call("cellSurMixerUtilNoteToRatio", 60, 72);
    CHECK(f32_result(g) == Catch::Approx(2.0f).epsilon(1e-6));
    g.call("cellSurMixerUtilNoteToRatio", 69, 57);
    CHECK(f32_result(g) == Catch::Approx(0.5f).epsilon(1e-6));
    g.call("cellSurMixerUtilNoteToRatio", 60, 60);
    CHECK(f32_result(g) == 1.0f);
    g.call("cellSurMixerUtilNoteToRatio", 60, 67); // a fifth
    CHECK(f32_result(g) == Catch::Approx(1.4983071f).epsilon(1e-6));
    g.call("cellSurMixerBeep", 1); // no system beep: nothing happens
}
