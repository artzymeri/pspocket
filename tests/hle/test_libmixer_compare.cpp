// libmixer against Sony's libmixer (D-563, D-597): the surround mixer, its strips and buses, the sample players and the
// utilities, the same calls on the same inputs to pspocket's library and to Sony's module, both over pspocket's
// cellAudio. Compared: every answer and output word; the audio port the mixer opens; the game's notify callback, each
// call's argument, tag and sample count, in order; the blocks the mixer writes into its port's ring and what the
// speakers hear after each change (the strips' levels and mutes, a bus, a looped player, the total level and mute, a
// player stopped); a player's first block at double speed, looped and slowed down, frame by frame; the timestamps;
// the utilities over a range of their arguments.
//
// What counts as the same sound (D-597): the inputs are exact in binary (halves, quarters, sixteenths, 1/32768ths), so
// both mixers' sums should be too, but Sony's may add them in another order or scale them once more: samples within
// 1e-6 (2^-20, far below a 16-bit step) count as the same; the counts within the tolerance are reported. When a block
// is heard (the latency after Start, the blocks mixed while the halves wait) depends on the threads' timing: the
// steady values are compared, after a change has had 100 ms to reach the speakers, and the latency is a note. The
// timestamps are compared relative to the first one, within 0.1%. No game data; skipped without the firmware
// (compare_util.hpp).

#include <catch_amalgamated.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "compare_util.hpp"
#include "pspocket/hle/audio.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;
using namespace std::chrono_literals;
namespace as = pspocket::recomp::ppu::as;

namespace {

constexpr u32 kConfig = kData + 0x100, kOut = kData + 0x140, kStrip = kData + 0x180, kTag = kData + 0x1C0,
              kStamp = kData + 0x1C8, kInfo = kData + 0x200, kWave = kData + 0x240, kCommon = kData + 0x260,
              kPlayerConfig = kData + 0x270, kAudioConfig = kData + 0x280, kCode = kData + 0x1000,
              kCallback = kData + 0x1F00, kMono = kData + 0x2000, kStereo = kData + 0x2400, kSur6 = kData + 0x2C00,
              kSur8 = kData + 0x4400, kBus = kData + 0x6400, kPcm = kData + 0x6800, kRecords = kData + 0x10000;
constexpr u32 kAan = 0x11111111, kRecordsKept = 1024; // the callback's records: a count, then 16 bytes each

// The game's notify callback, guest code (test_libmixer.cpp's, through the half's own exports): records (arg, counter,
// samples) at kRecords, then when `feed` adds the block's data: the mono, stereo, 7.1 and 5.1 strips (index 0 of each)
// through cellAANAddData and bus 3 through cellSurMixerSurBusAddData. Returns 0.
void write_callback(u8* b, u32 aan, u32 bus, bool feed)
{
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
    c.push_back(as::andi_(12, 12, kRecordsKept - 1));
    c.push_back(as::mulli(12, 12, 16));
    c.push_back(as::add(12, 11, 12));
    c.push_back(as::stw(3, 16, 12));
    c.push_back(as::stw(4, 20, 12));
    c.push_back(as::stw(5, 24, 12));
    if (feed && aan && bus) {
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
    std::memset(b + kRecords, 0, 16 + 16 * kRecordsKept);
}

// What the speakers hear: each period's 256 stereo frames (cellAudio's thread hands them over).
struct Speakers {
    std::mutex mutex;
    std::vector<std::vector<f32>> blocks;

    usize count()
    {
        std::lock_guard lock(mutex);
        return blocks.size();
    }
    std::vector<f32> block(usize i)
    {
        std::lock_guard lock(mutex);
        return i < blocks.size() ? blocks[i] : std::vector<f32>{};
    }
};

bool constant(const std::vector<f32>& block)
{
    for (usize i = 2; i + 1 < block.size(); i += 2)
        if (block[i] != block[0] || block[i + 1] != block[1]) return false;
    return !block.empty();
}

struct Run : Half<> {
    Speakers speakers;
    u32 port = ~0u; // the audio port the mixer opened

    Run(const Setup& s, bool sony_half) : Half(s, sony_half, "libmixer")
    {
        hle::set_audio_sink(*f.hle, [this](const f32* samples, u32 frames) {
            std::lock_guard lock(speakers.mutex);
            speakers.blocks.emplace_back(samples, samples + 2 * frames);
        });
    }
    ~Run() { hle::set_audio_sink(*f.hle, {}); }

    u64 r(std::string_view fn, std::initializer_list<u64> args, std::string key = {}) { return Half::r(fn, args, key); }

    // cellSurMixerSetParameter(param, float): the value in f1.
    u64 set_parameter(u32 param, f32 value, const std::string& key)
    {
        t.ctx.fpr[1] = static_cast<double>(value);
        return r("cellSurMixerSetParameter", {param}, key);
    }

    // cellSurMixerChStripSetParameter(type, index, {param, 0, dBSwitch, floatVal, intVal}).
    u64 strip(u32 type, u32 index, u32 param, bool db, f32 fv, s32 iv, const std::string& key)
    {
        ps_write32(b, kStrip, param);
        ps_write32(b, kStrip + 4, 0);
        ps_write32(b, kStrip + 8, db ? 1 : 0);
        ps_writef32(b, kStrip + 12, fv);
        ps_write32(b, kStrip + 16, static_cast<u32>(iv));
        return r("cellSurMixerChStripSetParameter", {type, index, kStrip}, key);
    }

    // A float utility: its answer in f1 (a float, as a double).
    double utility(std::string_view fn, std::initializer_list<u64> args, double f1 = 0)
    {
        t.ctx.fpr[1] = f1;
        if (!side.call(fn, args)) return std::nan("");
        return static_cast<double>(static_cast<f32>(t.ctx.fpr[1]));
    }

    u64 block_tag()
    {
        ps_write64(b, kTag, ~0ull);
        side.call("cellSurMixerGetCurrentBlockTag", {kTag});
        return ps_read64(b, kTag);
    }

    // The ports open now (pspocket's cellAudio in both halves): their numbers, states and shapes.
    std::string ports()
    {
        std::string s;
        for (u32 p = 0; p < 8; p++) {
            if (t.call("cellAudioGetPortConfig", p, kAudioConfig) != 0) continue;
            const u32 status = ps_read32(b, kAudioConfig + 4);
            if (status == 0x1010) continue;
            s += std::format("{}port {}: status {:#x}, {} channels, {} blocks, {:#x} bytes", s.empty() ? "" : "; ", p,
                status, ps_read64(b, kAudioConfig + 8), ps_read64(b, kAudioConfig + 16), ps_read32(b, kAudioConfig + 24));
        }
        return s.empty() ? "none" : s;
    }
    std::string port_status()
    {
        if (port == ~0u || t.call("cellAudioGetPortConfig", port, kAudioConfig) != 0) return "no port";
        return std::format("{:#x}", ps_read32(b, kAudioConfig + 4));
    }

    // What the speakers settle on after a change: 100 ms for it to come through, then four blocks in a row alike, each
    // alike within itself (at most a second more).
    void settle(const std::string& key)
    {
        std::this_thread::sleep_for(100ms);
        for (int tries = 0; tries < 200; tries++) {
            const usize n = speakers.count();
            if (n >= 4) {
                const std::vector<f32> last = speakers.block(n - 1);
                bool same = constant(last);
                for (usize k = 2; k <= 4 && same; k++) {
                    const std::vector<f32> x = speakers.block(n - k);
                    same = constant(x) && x[0] == last[0] && x[1] == last[1];
                }
                if (same) {
                    o.numbers(key, {static_cast<double>(last[0]), static_cast<double>(last[1])}, 1e-6);
                    return;
                }
            }
            std::this_thread::sleep_for(5ms);
        }
        o.line(key, "the speakers did not settle");
    }

    // The blocks the mixer's port holds: each block's eight channels when it is the same all through (else
    // "uneven"), the distinct ones that are not silent.
    void ring(const std::string& key)
    {
        if (port == ~0u || t.call("cellAudioGetPortConfig", port, kAudioConfig) != 0) {
            o.line(key, "no port");
            return;
        }
        const u32 addr = ps_read32(b, kAudioConfig + 0x1C);
        const u32 blocks = static_cast<u32>(ps_read64(b, kAudioConfig + 16)),
                  channels = static_cast<u32>(ps_read64(b, kAudioConfig + 8));
        std::vector<std::vector<double>> distinct;
        u32 uneven = 0;
        for (u32 k = 0; k < blocks; k++) {
            const u32 at = addr + k * channels * 256 * 4;
            std::vector<double> v(channels);
            bool even = true, silent = true;
            for (u32 c = 0; c < channels; c++) v[c] = static_cast<double>(ps_readf32(b, at + 4 * c));
            for (u32 i = 0; i < 256; i++)
                for (u32 c = 0; c < channels; c++) {
                    const f32 x = ps_readf32(b, at + 4 * (i * channels + c));
                    even = even && x == static_cast<f32>(v[c]);
                    silent = silent && x == 0.0f;
                }
            if (silent) continue;
            if (!even) {
                uneven++;
                continue;
            }
            if (std::find(distinct.begin(), distinct.end(), v) == distinct.end()) distinct.push_back(v);
        }
        o.line(key + ": blocks not alike all through", std::format("{}", uneven));
        o.line(key + ": distinct blocks", std::format("{}", distinct.size()));
        for (usize i = 0; i < distinct.size(); i++) o.numbers(std::format("{}: block {}", key, i), distinct[i], 1e-6);
    }

    // The callback's records: how many, the first `n`, whether the counters go on one by one.
    void records(const std::string& key, u32 n)
    {
        const u32 count = ps_read32(b, kRecords);
        o.note(std::format("{}: {} calls of the notify callback", key, count));
        for (u32 i = 0; i < std::min(n, count); i++) {
            const u32 at = kRecords + 16 + 16 * (i % kRecordsKept);
            o.line(std::format("{}: call {}", key, i), std::format("arg {:#x} counter {} samples {}", ps_read32(b, at),
                                                           ps_read32(b, at + 4), ps_read32(b, at + 8)));
        }
        bool consecutive = count > 0;
        const u32 first = ps_read32(b, kRecords + 20);
        for (u32 i = 1; i < std::min(count, kRecordsKept); i++)
            consecutive = consecutive && ps_read32(b, kRecords + 16 + 16 * i + 4) == first + i;
        o.line(key + ": the counters go on one by one", consecutive ? "yes" : "no");
    }
};

// The mixer's life and a block's mix (test_libmixer.cpp's first two cases).
Observed life(const Setup& s, bool sony_half)
{
    Run h(s, sony_half);
    u8* b = h.b;
    write_callback(b, h.side.descriptor("cellAANAddData"), h.side.descriptor("cellSurMixerSurBusAddData"), true);
    ps_write32(b, kConfig, 100);
    for (u32 i = 0; i < 4; i++) ps_write32(b, kConfig + 4 + 4 * i, 1);
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
    for (u32 i = 0; i < 512; i++) {
        ps_write8(b, kPcm + 2 * i, 0x00);
        ps_write8(b, kPcm + 2 * i + 1, 0x40);
    }

    // Before cellAudioInit.
    h.r("cellSurMixerCreate", {kConfig}, "Create before cellAudioInit");
    h.r("cellSurMixerStart", {}, "Start before Create");
    h.r("cellSurMixerPause", {1}, "Pause before Create");
    h.r("cellSurMixerFinalize", {}, "Finalize before Create");
    h.set_parameter(8, 0.5f, "SetParameter before Create");
    std::memset(b + kTag, 0xEE, 16);
    h.r("cellSurMixerGetCurrentBlockTag", {kTag}, "GetCurrentBlockTag before Create");
    h.o.line("GetCurrentBlockTag before Create: the tag", hex(b + kTag, 8));
    h.o.line("cellAudioInit", answer(h.t.call("cellAudioInit")));

    // Create, the port it opens, the AAN's handle and port numbers.
    h.r("cellSurMixerCreate", {kConfig}, "Create");
    h.r("cellSurMixerCreate", {kConfig}, "Create again");
    h.o.line("the ports open after Create", h.ports());
    for (u32 p = 0; p < 8; p++)
        if (h.t.call("cellAudioGetPortConfig", p, kAudioConfig) == 0 && ps_read32(b, kAudioConfig + 4) != 0x1010) {
            h.port = p;
            break;
        }
    std::memset(b + kOut, 0xEE, 8);
    h.r("cellSurMixerGetAANHandle", {kOut}, "GetAANHandle");
    h.o.line("GetAANHandle: the handle", hex(b + kOut, 8));
    h.r("cellSurMixerGetAANHandle", {0}, "GetAANHandle(NULL)");
    static constexpr std::pair<u32, u32> kStrips[] = {{1, 0}, {2, 0}, {3, 0}, {4, 2}, {5, 0}, {1, 7}};
    for (const auto& [type, index] : kStrips) {
        std::memset(b + kOut, 0xEE, 8);
        const std::string key = std::format("ChStripGetAANPortNo(type {}, index {})", type, index);
        h.r("cellSurMixerChStripGetAANPortNo", {kOut, type, index}, key);
        h.o.line(key + ": the port number", hex(b + kOut, 8));
    }
    h.r("cellSurMixerChStripGetAANPortNo", {0, 1, 0}, "ChStripGetAANPortNo(NULL)");

    // The notify callback: one at a time.
    h.r("cellSurMixerSetNotifyCallback", {0, 0}, "SetNotifyCallback(NULL)");
    h.r("cellSurMixerSetNotifyCallback", {kCallback, 0xC0FFEE}, "SetNotifyCallback");
    h.r("cellSurMixerSetNotifyCallback", {kCallback, 0}, "SetNotifyCallback again");
    h.r("cellSurMixerRemoveNotifyCallback", {kCallback + 8}, "RemoveNotifyCallback of another");
    h.r("cellSurMixerRemoveNotifyCallback", {kCallback}, "RemoveNotifyCallback");
    h.r("cellSurMixerRemoveNotifyCallback", {kCallback}, "RemoveNotifyCallback again");
    h.r("cellSurMixerSetNotifyCallback", {kCallback, 0xC0FFEE}, "SetNotifyCallback once more");
    std::this_thread::sleep_for(30ms);
    h.o.line("callback calls before Start", std::format("{}", ps_read32(b, kRecords)));
    h.o.line("the block tag before Start", std::format("{:#x}", h.block_tag()));

    // The strips: the mono one at level 0.5, the 7.1 one at expression 0.5 (in dB), the 5.1 one muted; the other
    // parameters; the total level 0.5.
    h.strip(1, 0, 0, false, 0.5f, 0, "strip mono 0: level 0.5");
    h.strip(4, 0, 1, true, 20.0f * std::log10(0.5f), 0, "strip 7.1 0: expression 0.5 in dB");
    h.strip(3, 0, 4, false, 0.0f, 1, "strip 5.1 0: muted");
    for (u32 param = 2; param <= 6; param++)
        if (param != 4) h.strip(1, 0, param, false, 0.0f, 0, std::format("strip mono 0: parameter {} (0)", param));
    h.strip(4, 0, 6, false, 0.0f, 0, "strip 7.1 0: parameter 6 (a position)");
    h.strip(2, 0, 7, false, 0.0f, 0, "strip stereo 0: parameter 7");
    h.strip(1, 1, 0, false, 1.0f, 0, "strip mono 1 (none made)");
    h.strip(5, 0, 0, false, 1.0f, 0, "strip of type 5");
    h.r("cellSurMixerChStripSetParameter", {1, 0, 0}, "ChStripSetParameter(NULL)");
    h.set_parameter(40, 0.0f, "SetParameter(total level 0 dB)");
    h.set_parameter(9, 0.5f, "SetParameter(reverb level 0.5)");
    h.set_parameter(41, -6.0f, "SetParameter(reverb level -6 dB)");
    h.set_parameter(12, 0.0f, "SetParameter(total mute off)");
    h.set_parameter(99, 1.0f, "SetParameter(99)");
    h.set_parameter(8, 0.5f, "SetParameter(total level 0.5)");

    // The player: one channel, the wave looped from its start, connected, at level 0.5.
    ps_write32(b, kPlayerConfig, 3);
    ps_write32(b, kPlayerConfig + 4, 0);
    h.r("cellSSPlayerCreate", {kOut, kPlayerConfig}, "SSPlayerCreate of three channels");
    ps_write32(b, kPlayerConfig, 1);
    ps_write32(b, kPlayerConfig + 4, 1);
    h.r("cellSSPlayerCreate", {kOut, kPlayerConfig}, "SSPlayerCreate of output mode 1");
    ps_write32(b, kPlayerConfig + 4, 0);
    std::memset(b + kOut, 0xEE, 8);
    const u64 created = h.r("cellSSPlayerCreate", {kOut, kPlayerConfig}, "SSPlayerCreate");
    const u32 player = ps_read32(b, kOut);
    h.o.line("SSPlayerCreate: the handle", hex(b + kOut, 8));
    if (created != 0) return std::move(h.o);
    ps_write32(b, kWave, kPcm);
    ps_write32(b, kWave + 4, 0);
    ps_write32(b, kWave + 8, 512);
    ps_write32(b, kWave + 12, 1);
    ps_write32(b, kWave + 16, 1);
    ps_write32(b, kCommon, 16); // CELL_SSPLAYER_LOOP_ON
    ps_write32(b, kCommon + 4, 0);
    h.r("cellSSPlayerSetWave", {player, kWave, kCommon}, "SSPlayerSetWave");
    h.r("cellAANConnect", {kAan, 0, player, 0}, "AANConnect");
    h.r("cellSSPlayerGetState", {player}, "SSPlayerGetState before Play");
    ps_writef32(b, kInfo, 0.5f);
    ps_writef32(b, kInfo + 4, 1.0f);
    for (u32 i = 2; i < 5; i++) ps_writef32(b, kInfo + 4 * i, 0.0f);
    h.r("cellSSPlayerPlay", {player, kInfo}, "SSPlayerPlay");
    h.r("cellSSPlayerGetState", {player}, "SSPlayerGetState after Play");

    // Start: a block a period, the callback first.
    const usize heard_before = h.speakers.count();
    const u64 started = lv2::guest_system_time_us();
    h.r("cellSurMixerStart", {}, "Start");
    h.r("cellSurMixerStart", {}, "Start again");
    h.o.line("the mixer's port after Start", h.port_status());
    bool mixed = false;
    for (int tries = 0; tries < 600 && !mixed; tries++) {
        mixed = h.block_tag() >= 8;
        if (!mixed) std::this_thread::sleep_for(5ms);
    }
    h.o.line("eight blocks mixed within 3 s", mixed ? "yes" : "no");
    {
        usize first = 0;
        for (usize i = heard_before; i < h.speakers.count() && !first; i++)
            if (h.speakers.block(i)[0] != 0.0f) first = i;
        h.o.note(std::format("the first block heard {} periods after Start", first ? first - heard_before : 0));
    }

    // Timestamps: the first, and the others after it.
    h.r("cellSurMixerGetTimestamp", {0, kStamp}, "GetTimestamp(0)");
    const u64 t0 = ps_read64(b, kStamp);
    h.o.note(std::format("GetTimestamp(0) {} us after the call of Start", static_cast<s64>(t0 - started)));
    for (const u64 tag : {1ull, 3ull, 100ull, 1000ull}) {
        h.r("cellSurMixerGetTimestamp", {tag, kStamp}, std::format("GetTimestamp({})", tag));
        h.o.numbers(std::format("GetTimestamp({}) after GetTimestamp(0), us", tag),
            {static_cast<double>(static_cast<s64>(ps_read64(b, kStamp) - t0))}, 1e-3);
    }
    h.r("cellSurMixerGetTimestamp", {0, 0}, "GetTimestamp(0, NULL)");

    // The steady mix at the speakers, then in the port's ring with the mixer paused.
    h.settle("the speakers with every strip, the bus and the player");
    {
        bool even = true;
        for (usize i = heard_before; i < h.speakers.count(); i++) even = even && constant(h.speakers.block(i));
        h.o.line("every block heard alike all through", even ? "yes" : "no");
    }
    h.r("cellSurMixerPause", {1}, "Pause(1)");
    h.o.line("the mixer's port after Pause(1)", h.port_status());
    std::this_thread::sleep_for(20ms);
    const u64 paused = h.block_tag();
    std::this_thread::sleep_for(40ms);
    h.o.line("no block mixed while paused", h.block_tag() == paused ? "yes" : "no");
    h.ring("the port's ring, paused");
    h.r("cellSurMixerPause", {3}, "Pause(3)");
    h.r("cellSurMixerPause", {0}, "Pause(0)");
    h.o.line("the mixer's port after Pause(0)", h.port_status());
    std::this_thread::sleep_for(50ms);
    h.o.line("blocks mixed again after Pause(0)", h.block_tag() > paused ? "yes" : "no");

    // The total mute and back; the player stopped and removed.
    h.set_parameter(12, 1.0f, "SetParameter(total mute on)");
    h.settle("the speakers, muted");
    h.set_parameter(12, 0.0f, "SetParameter(total mute off) again");
    h.settle("the speakers, not muted");
    h.set_parameter(8, 0.25f, "SetParameter(total level 0.25)");
    h.settle("the speakers at total level 0.25");
    h.r("cellSSPlayerStop", {player, 0}, "SSPlayerStop");
    h.r("cellSSPlayerGetState", {player}, "SSPlayerGetState after Stop");
    h.settle("the speakers without the player");
    h.r("cellAANDisconnect", {kAan, 0, player, 0}, "AANDisconnect");
    h.r("cellSSPlayerRemove", {player}, "SSPlayerRemove");
    h.r("cellSSPlayerGetState", {player}, "SSPlayerGetState after Remove");

    // Finalize: no more callbacks, the port closed; the records; a new mixer.
    h.r("cellSurMixerFinalize", {}, "Finalize");
    const u32 calls = ps_read32(b, kRecords);
    std::this_thread::sleep_for(30ms);
    h.o.line("no callback after Finalize", ps_read32(b, kRecords) == calls ? "yes" : "no");
    h.o.line("the mixer's port after Finalize", h.port_status());
    h.o.line("the ports open after Finalize", h.ports());
    h.records("the notify callback", 8);
    h.r("cellSurMixerFinalize", {}, "Finalize again");
    h.r("cellSurMixerCreate", {kConfig}, "Create after Finalize");
    h.o.line("the ports open after the second Create", h.ports());
    h.r("cellSurMixerFinalize", {}, "Finalize of the second mixer");
    h.o.line("cellAudioQuit", answer(h.t.call("cellAudioQuit")));
    return std::move(h.o);
}

// A sample player's speed, loops and stereo waves (test_libmixer.cpp's third case), and the utilities.
Observed players(const Setup& s, bool sony_half)
{
    Run h(s, sony_half);
    u8* b = h.b;
    ps_write32(b, kConfig, 100);
    for (u32 i = 0; i < 4; i++) ps_write32(b, kConfig + 4 + 4 * i, 0);
    h.o.line("cellAudioInit", answer(h.t.call("cellAudioInit")));
    h.r("cellSurMixerCreate", {kConfig}, "Create with no strips");
    h.r("cellSurMixerStart", {}, "Start");

    // A stereo wave of 100 frames: left i, right -i (in 1/32768ths).
    for (u32 i = 0; i < 100; i++) {
        const u16 left = static_cast<u16>(i), right = static_cast<u16>(-static_cast<s32>(i));
        ps_write8(b, kPcm + 4 * i, static_cast<u8>(left));
        ps_write8(b, kPcm + 4 * i + 1, static_cast<u8>(left >> 8));
        ps_write8(b, kPcm + 4 * i + 2, static_cast<u8>(right));
        ps_write8(b, kPcm + 4 * i + 3, static_cast<u8>(right >> 8));
    }
    ps_write32(b, kPlayerConfig, 2);
    ps_write32(b, kPlayerConfig + 4, 0);
    std::memset(b + kOut, 0xEE, 8);
    if (h.r("cellSSPlayerCreate", {kOut, kPlayerConfig}, "SSPlayerCreate of two channels") != 0) return std::move(h.o);
    const u32 player = ps_read32(b, kOut);
    h.o.line("SSPlayerCreate: the handle", hex(b + kOut, 8));
    h.r("cellAANConnect", {kAan, 0, player, 0}, "AANConnect");

    // The first block of the player's sound at the speakers (one whose first frame is not silent), all 256 frames.
    const auto first_sound = [&](const std::string& key, usize from) {
        for (int tries = 0; tries < 400; tries++) {
            for (usize i = from; i < h.speakers.count(); i++) {
                const std::vector<f32> block = h.speakers.block(i);
                if (block[0] == 0.0f && block[1] == 0.0f) continue;
                std::vector<u8> bytes(4 * block.size());
                for (usize k = 0; k < block.size(); k++) ps_st32(bytes.data() + 4 * k, std::bit_cast<u32>(block[k]));
                h.o.bytes(key, std::move(bytes), Rule::floats(1e-6));
                return;
            }
            std::this_thread::sleep_for(5ms);
        }
        h.o.line(key, "no sound within 2 s");
    };
    const auto state_becomes = [&](u32 want, const std::string& key) {
        u64 state = 0;
        for (int tries = 0; tries < 400; tries++) {
            state = h.side.call("cellSSPlayerGetState", {player}).value_or(~0ull);
            if (static_cast<u32>(state) == want) break;
            std::this_thread::sleep_for(5ms);
        }
        h.o.line(key, answer(state));
    };
    const auto play = [&](const std::string& what, u32 start, u32 loop_start, s32 loop_mode, f32 speed) {
        ps_write32(b, kWave, kPcm);
        ps_write32(b, kWave + 4, 0);
        ps_write32(b, kWave + 8, 100);
        ps_write32(b, kWave + 12, loop_start);
        ps_write32(b, kWave + 16, start);
        ps_write32(b, kCommon, static_cast<u32>(loop_mode));
        ps_write32(b, kCommon + 4, 0);
        h.r("cellSSPlayerSetWave", {player, kWave, loop_mode < 0 ? 0u : kCommon}, "SSPlayerSetWave " + what);
        ps_writef32(b, kInfo, 1.0f);
        ps_writef32(b, kInfo + 4, speed);
        for (u32 i = 2; i < 5; i++) ps_writef32(b, kInfo + 4 * i, 0.0f);
        const usize from = h.speakers.count();
        h.r("cellSSPlayerPlay", {player, kInfo}, "SSPlayerPlay " + what);
        first_sound("the first block heard, " + what, from);
    };
    // Once at double speed from its 11th frame, no common parameters (a one shot), until it ends.
    play("once at double speed from frame 11", 11, 1, -1, 2.0f);
    state_becomes(0x00, "SSPlayerGetState once it ended");
    // Looped at normal speed from frame 0, the loop from frame 90.
    play("looped from frame 90", 1, 91, 16, 1.0f);
    h.r("cellSSPlayerGetState", {player}, "SSPlayerGetState looping");
    h.r("cellSSPlayerStop", {player, 0}, "SSPlayerStop of the loop");
    // Slower (half speed, repeating frames), looped; held at its last frame (ONESHOT_CONT).
    play("looped at half speed", 1, 1, 16, 0.5f);
    h.r("cellSSPlayerStop", {player, 0}, "SSPlayerStop at half speed");
    play("once, held at the end, from frame 90", 91, 1, 2, 1.0f);
    std::this_thread::sleep_for(50ms);
    h.r("cellSSPlayerGetState", {player}, "SSPlayerGetState held at the end");
    h.r("cellSSPlayerStop", {player, 0}, "SSPlayerStop held at the end");
    // SetParam: the level and speed while playing.
    play("looped, before SetParam", 1, 1, 16, 1.0f);
    ps_writef32(b, kInfo, 0.5f);
    ps_writef32(b, kInfo + 4, 1.0f);
    const usize from = h.speakers.count();
    h.r("cellSSPlayerSetParam", {player, kInfo}, "SSPlayerSetParam level 0.5");
    std::this_thread::sleep_for(100ms);
    {
        // A block after the change: its largest sample (the loop's frame 99 at half level).
        f32 peak = 0;
        const std::vector<f32> block = h.speakers.block(std::max(from, h.speakers.count() - 1));
        for (const f32 v : block) peak = std::max(peak, std::fabs(v));
        h.o.numbers("the loudest sample after SetParam", {static_cast<double>(peak)}, 1e-6);
    }
    h.r("cellSSPlayerStop", {player, 0}, "SSPlayerStop after SetParam");
    // A start past the wave's end.
    ps_write32(b, kWave + 16, 200);
    h.r("cellSSPlayerSetWave", {player, kWave, kCommon}, "SSPlayerSetWave from frame 200 of 100");
    h.r("cellSSPlayerPlay", {player, kInfo}, "SSPlayerPlay from frame 200 of 100");
    state_becomes(0x00, "SSPlayerGetState from frame 200 of 100");
    h.r("cellSSPlayerStop", {player, 0}, "SSPlayerStop from frame 200 of 100");
    h.r("cellSSPlayerRemove", {player}, "SSPlayerRemove");

    h.r("cellSurMixerFinalize", {}, "Finalize");
    h.o.line("cellAudioQuit", answer(h.t.call("cellAudioQuit")));

    // The utilities: levels from decibels and from a decibel index (pspocket takes the index as whole decibels, the
    // first thing to check), note ratios.
    h.o.group("cellSurMixerUtilGetLevelFromDB");
    for (const double db : {0.0, -20.0, -6.0, 6.0, 20.0 * std::log10(2.0), -100.0, -96.0, 12.0, 40.0, -40.0, 0.5, -0.5})
        h.o.numbers(std::format("GetLevelFromDB({})", db), {h.utility("cellSurMixerUtilGetLevelFromDB", {}, db)}, 1e-6);
    h.o.group("cellSurMixerUtilGetLevelFromDBIndex");
    for (s32 index = -200; index <= 200; index++)
        h.o.numbers(std::format("GetLevelFromDBIndex({})", index),
            {h.utility("cellSurMixerUtilGetLevelFromDBIndex", {static_cast<u64>(static_cast<u32>(index))})}, 1e-6);
    h.o.group("cellSurMixerUtilNoteToRatio");
    static constexpr std::pair<u32, u32> kNotes[] = {{60, 72}, {69, 57}, {60, 60}, {60, 67}, {0, 127}, {127, 0},
        {60, 61}, {48, 60}, {60, 300}, {255, 0}};
    for (const auto& [ref, note] : kNotes)
        h.o.numbers(std::format("NoteToRatio({}, {})", ref, note), {h.utility("cellSurMixerUtilNoteToRatio", {ref, note})},
            1e-6);
    h.o.group("");
    return std::move(h.o);
}

} // namespace

TEST_CASE("libmixer's comparison calls Sony's exports by their names; pspocket's library, firmware first",
    "[hle][libmixer]")
{
    check_names("libmixer",
        {"cellAANAddData", "cellAANConnect", "cellAANDisconnect", "cellSurMixerCreate", "cellSurMixerGetAANHandle",
            "cellSurMixerChStripGetAANPortNo", "cellSurMixerSetNotifyCallback", "cellSurMixerRemoveNotifyCallback",
            "cellSurMixerStart", "cellSurMixerSetParameter", "cellSurMixerFinalize", "cellSurMixerSurBusAddData",
            "cellSurMixerChStripSetParameter", "cellSurMixerPause", "cellSurMixerGetCurrentBlockTag",
            "cellSurMixerGetTimestamp", "cellSSPlayerCreate", "cellSSPlayerRemove", "cellSSPlayerSetWave",
            "cellSSPlayerPlay", "cellSSPlayerStop", "cellSSPlayerSetParam", "cellSSPlayerGetState",
            "cellSurMixerUtilGetLevelFromDB", "cellSurMixerUtilGetLevelFromDBIndex", "cellSurMixerUtilNoteToRatio"});
}

TEST_CASE("libmixer against Sony's libmixer: the mixer's life, a block's mix, the port, the callback",
    "[.][hle][lle][compare][libmixer]")
{
    const Setup s = setup();
    run(s, "libmixer (the mixer's life and mix) against Sony's libmixer", [&](bool sony) { return life(s, sony); },
        [](const Observed& o) {
            // pspocket's own half: test_libmixer.cpp's answers and mix.
            CHECK(o.value("Create before cellAudioInit") == "0x80310002");
            CHECK(o.value("Create") == "0x0");
            CHECK(o.value("Create again") == "0x80310006");
            CHECK(o.value("the ports open after Create") == "port 0: status 0x1, 8 channels, 16 blocks, 0x20000 bytes");
            CHECK(o.value("GetAANHandle: the handle") == "hex:11111111eeeeeeee");
            CHECK(o.value("ChStripGetAANPortNo(type 4, index 2): the port number") == "hex:00040002eeeeeeee");
            CHECK(o.value("SetNotifyCallback again") == "0x80310006");
            CHECK(o.value("strip mono 1 (none made)") == "0x80310003");
            CHECK(o.value("eight blocks mixed within 3 s") == "yes");
            // The block (test_libmixer.cpp's sum): (0.25 + 0.25 + 1/16 + voice) / 2 on the left, voice = 0.25.
            const f32 expression = std::pow(10.0f, 20.0f * std::log10(0.5f) / 20.0f);
            const f32 left = (0.5f * 0.5f + 0.25f + 0.0625f * expression + 0.25f) * 0.5f;
            const f32 right = (0.5f * 0.5f - 0.125f + 0.125f * expression + 0.25f) * 0.5f;
            CHECK(o.value("the speakers with every strip, the bus and the player") ==
                std::format("{} {}", number(left), number(right)));
            CHECK(o.value("the speakers, muted") == "0 0");
            CHECK(o.value("the port's ring, paused: distinct blocks") == "1");
            CHECK(o.value("no block mixed while paused") == "yes");
            CHECK(o.value("SSPlayerGetState after Remove") == "0xffffffff");
            CHECK(o.value("no callback after Finalize") == "yes");
            CHECK(o.value("the mixer's port after Finalize") == "0x1010");
            CHECK(o.value("the notify callback: call 3") == "arg 0xc0ffee counter 3 samples 256");
            CHECK(o.value("the notify callback: the counters go on one by one") == "yes");
            CHECK(o.value("GetTimestamp(3) after GetTimestamp(0), us") == "16000");
        });
}

TEST_CASE("libmixer against Sony's libmixer: sample players' speeds and loops, the utilities",
    "[.][hle][lle][compare][libmixer]")
{
    const Setup s = setup();
    run(s, "libmixer (players and utilities) against Sony's libmixer", [&](bool sony) { return players(s, sony); },
        [](const Observed& o) {
            const Data* once = o.find_data("the first block heard, once at double speed from frame 11");
            REQUIRE(once);
            REQUIRE(once->bytes.size() == 2 * 256 * 4);
            // RPCS3's rough speed change: output sample k holds frames 10 + 2k and 11 + 2k (test_libmixer.cpp).
            CHECK(std::bit_cast<f32>(ps_ld32(once->bytes.data())) == 21.0f / 32768.0f);
            CHECK(std::bit_cast<f32>(ps_ld32(once->bytes.data() + 8)) == 25.0f / 32768.0f);
            CHECK(o.value("SSPlayerGetState once it ended") == "0x0");
            CHECK(o.value("SSPlayerGetState looping") == "0x20");
            CHECK(o.value("SSPlayerGetState from frame 200 of 100") == "0x0");
            CHECK(o.value("GetLevelFromDB(-20)") == number(0.1f));
            CHECK(o.value("GetLevelFromDBIndex(-20)") == number(0.1f));
            CHECK(o.value("NoteToRatio(60, 72)") == "2");
        });
}
