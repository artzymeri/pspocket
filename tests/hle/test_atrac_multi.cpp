// cellAtracMulti (D-535) against libatrac3multi, Sony's own module, from the user's installed system software
// (firmware_compare.hpp; skipped without it): ATRAC3 files of several channels built here (a RIFF WAVE header, silent
// frames: each channel's block a sound unit with no spectrum) played whole, half loaded, streamed, looped and with a
// second buffer, with output tracks taken from the file's channels in orders the test picks (silent tracks among them);
// the frames go to cellAdec's ATRAC3 multichannel core from the same firmware. The answers, every output, the handle's
// bytes (but the lv2 IDs, cellAdec's handle and the callback's descriptor), the shuffled frame and the PCM must be the
// same. And headers with each thing wrong. No game data.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "firmware_compare.hpp"
#include "hle_test_util.hpp"
#include "pspocket/core/be.h"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kHandle = kData + 0x200, kOut = kData + 0x500, kExt = kData + 0x480, kTracks = kData + 0x600;
constexpr u32 kFile = kData + 0x10000, kBuffer = kData + 0x60000, kSecond = kData + 0x80000;
constexpr u32 kWork = kData + 0x90000, kPcm = kData + 0x100000;
constexpr u32 kBlock = 0xC0; // one channel's block of a frame

struct FileSpec {
    u32 channels = 4;
    u32 frames = 24;
    s32 samples = -1;    // the fact chunk's samples (-1: no fact)
    s32 offset = -1;     // the fact chunk's first sample offset (-1: a 4-byte fact)
    s32 loop_start = -1; // smpl's loop (-1: none)
    s32 loop_end = -1;
    u32 rate = 44100;
    u16 tag = 0x270;
    u16 joint = 0;
};

// An ATRAC3 file of `channels` channels: RIFF WAVE, fmt (0x270 with its 14 extra bytes), fact, smpl, data. Each frame
// is a block per channel: a sound unit's header (0xA2) and nothing coded, with a byte that differs per channel and
// frame past it, so a block in the wrong place shows in the frame cellAdec gets.
std::vector<u8> at3_file(const FileSpec& s)
{
    std::vector<u8> f;
    const auto u16le = [&](u32 v) {
        f.push_back(static_cast<u8>(v));
        f.push_back(static_cast<u8>(v >> 8));
    };
    const auto u32le = [&](u32 v) {
        u16le(v & 0xFFFF);
        u16le(v >> 16);
    };
    const auto tag = [&](const char* t) { f.insert(f.end(), t, t + 4); };
    const u32 frame = kBlock * s.channels;
    tag("RIFF");
    u32le(0);
    tag("WAVE");
    tag("fmt ");
    u32le(0x20);
    u16le(s.tag);
    u16le(s.channels);
    u32le(s.rate);
    u32le(s.rate * frame / 1024);
    u16le(frame);
    u16le(0);
    u16le(14);
    u16le(1);
    u32le(0x1000);
    u16le(s.joint);
    u16le(s.joint);
    u16le(1);
    u16le(0);
    if (s.samples >= 0) {
        tag("fact");
        u32le(s.offset >= 0 ? 8 : 4);
        u32le(static_cast<u32>(s.samples));
        if (s.offset >= 0) u32le(static_cast<u32>(s.offset));
    }
    if (s.loop_start >= 0) {
        tag("smpl");
        u32le(0x3C);
        for (u32 i = 0; i < 7; i++) u32le(0);
        u32le(1);
        u32le(0);
        u32le(0);
        u32le(0);
        u32le(static_cast<u32>(s.loop_start));
        u32le(static_cast<u32>(s.loop_end));
        u32le(0);
        u32le(0);
    }
    tag("data");
    u32le(s.frames * frame);
    for (u32 i = 0; i < s.frames; i++) {
        for (u32 c = 0; c < s.channels; c++) {
            std::vector<u8> block(kBlock, 0);
            block[0] = 0xA2;
            block[kBlock - 1] = static_cast<u8>(0x10 * c + i);
            f.insert(f.end(), block.begin(), block.end());
        }
    }
    const u32 riff = static_cast<u32>(f.size() - 8);
    std::memcpy(f.data() + 4, &riff, 4);
    return f;
}

using Call = std::function<u32(const char*, std::initializer_list<u64>)>;

// The handle's bytes the comparison reads: all but the lv2 IDs, cellAdec's handle and the callback's descriptor.
std::string handle_bytes(u8* b)
{
    std::string s;
    for (u32 off = 0; off < 0x200; off += 4) {
        if (off == 0 || off == 4 || off == 0x20 || off == 0x3C) continue;
        if (const u32 w = ps_read32(b, kHandle + off)) s += std::format(" {:x}={:x}", off, w);
    }
    return s;
}

// The frame cellAdec got last: the shuffled buffer's bytes at each block's start and end.
std::string au_bytes(u8* b, u32 tracks)
{
    const u32 au = ps_read32(b, kHandle + 0x54);
    if (au < kWork || au >= kWork + 0x60000) return "au -";
    std::string s = "au";
    for (u32 t = 0; t < tracks; t++) s += std::format(" {:x}/{:x}", b[au + kBlock * t], b[au + kBlock * t + kBlock - 1]);
    return s;
}

struct Play {
    u32 first, buffer_size;
    s32 loops = 0;
    bool ext = false;
    u32 tracks = 2;
    std::vector<s32> array = {}; // empty: NULL (the file's channels in order)
};

// One play-through, the stream topped up as the library asks, loops set, a reset with a new track array at the end;
// the log of everything it answers.
std::vector<std::string> play(u8* b, const Call& call, const std::vector<u8>& file, const Play& p)
{
    std::vector<std::string> log;
    std::memset(b + kHandle, 0, 0x200);
    std::memset(b + kBuffer, 0xEE, 0x20000);
    std::memset(b + kOut, 0, 0x40);
    std::memset(b + kWork, 0x5A, 0x60000);
    std::memcpy(b + kFile, file.data(), file.size());
    for (u32 i = 0; i < p.array.size(); i++) ps_write32(b, kTracks + 4 * i, static_cast<u32>(p.array[i]));
    const u32 read = std::min<u32>(p.first, static_cast<u32>(file.size()));
    std::memcpy(b + kBuffer, file.data(), read);
    const auto step = [&](const char* name, std::initializer_list<u64> args) {
        const u32 r = call(name, args);
        std::string line = std::format("{} -> {:#x}", name, r);
        for (u32 i = 0; i < 8; i++) line += std::format(" {:x}", ps_read32(b, kOut + 4 * i));
        log.push_back(line);
        return r;
    };
    const u32 array = p.array.empty() ? 0 : kTracks;
    if (step("cellAtracMultiSetDataAndGetMemSize", {kHandle, kBuffer, read, p.buffer_size, p.tracks, array, kOut}) != 0) {
        log.push_back(handle_bytes(b));
        return log;
    }
    log.push_back(handle_bytes(b));
    std::memset(b + kOut + 0x20, 0xAB, 0x20);
    step("cellAtracMultiGetTrackArray", {kHandle, kOut + 0x20});
    if (p.ext) {
        ps_write32(b, kExt, 0);
        for (u32 i = 0; i < 8; i++) b[kExt + 4 + i] = static_cast<u8>(i + 1);
        step("cellAtracMultiCreateDecoderExt", {kHandle, kWork, 1000, kExt});
    } else {
        step("cellAtracMultiCreateDecoder", {kHandle, kWork, 1000, 100});
    }
    log.push_back(handle_bytes(b));
    for (const char* q : {"cellAtracMultiGetChannel", "cellAtracMultiGetMaxSample", "cellAtracMultiGetSamplingRate",
             "cellAtracMultiGetBitrate", "cellAtracMultiGetNextSample", "cellAtracMultiGetRemainFrame",
             "cellAtracMultiGetVacantSize", "cellAtracMultiGetNextDecodePosition", "cellAtracMultiGetInternalErrorInfo"})
        step(q, {kHandle, kOut});
    step("cellAtracMultiGetSoundInfo", {kHandle, kOut, kOut + 4, kOut + 8});
    step("cellAtracMultiIsSecondBufferNeeded", {kHandle});
    step("cellAtracMultiGetSecondBufferInfo", {kHandle, kOut, kOut + 4});
    if (ps_read32(b, kOut + 4)) {
        const u32 from = ps_read32(b, kOut), size = ps_read32(b, kOut + 4);
        std::memcpy(b + kSecond, file.data() + from, std::min<u32>(size, static_cast<u32>(file.size()) - from));
        step("cellAtracMultiSetSecondBuffer", {kHandle, kSecond, 0x100});
        step("cellAtracMultiSetSecondBuffer", {kHandle, kSecond, size});
    }
    step("cellAtracMultiSetLoopNum", {kHandle, static_cast<u32>(p.loops)});
    step("cellAtracMultiGetLoopInfo", {kHandle, kOut, kOut + 4});
    for (u32 n = 0; n < 200; n++) {
        step("cellAtracMultiGetStreamDataInfo", {kHandle, kOut + 0x10, kOut + 0x14, kOut + 0x18});
        const u32 write = ps_read32(b, kOut + 0x10), size = ps_read32(b, kOut + 0x14), pos = ps_read32(b, kOut + 0x18);
        const u32 frame = kBlock * 4;
        if (size && pos < file.size() && write >= kBuffer && write < kBuffer + 0x20000 && (n < 12 || n % 3 != 2)) {
            const u32 take =
                std::min({size, static_cast<u32>(file.size()) - pos, n < 12 ? 3 * frame + n % 5 * 0x40 : frame});
            std::memcpy(b + write, file.data() + pos, take);
            step("cellAtracMultiAddStreamData", {kHandle, take});
        }
        std::memset(b + kPcm, 0xCC, 0x8000);
        const u32 r = step("cellAtracMultiDecode", {kHandle, kPcm, kOut, kOut + 4, kOut + 8});
        log.push_back(au_bytes(b, p.tracks));
        std::string pcm = "pcm";
        for (u32 i = 0; i < 0x8000; i += 0x400) pcm += std::format(" {:x}", ps_read32(b, kPcm + i));
        log.push_back(pcm);
        step("cellAtracMultiGetVacantSize", {kHandle, kOut + 0xC});
        step("cellAtracMultiGetNextDecodePosition", {kHandle, kOut + 0xC});
        log.push_back(handle_bytes(b));
        if ((r != 0 && r != 0x80610B32) || ps_read32(b, kOut + 4)) break;
    }
    std::memset(b + kOut + 0x10, 0, 0x10);
    const u32 info = step("cellAtracMultiGetBufferInfoForResetting", {kHandle, 0x300, kOut + 0x10});
    const u32 at = ps_read32(b, kOut + 0x10), pos = ps_read32(b, kOut + 0x1C);
    if (info == 0 && ps_read32(b, kOut + 0x14) && pos < file.size() && at >= kBuffer && at < kBuffer + 0x20000) {
        const u32 size = std::min<u32>(ps_read32(b, kOut + 0x14), static_cast<u32>(file.size()) - pos);
        std::memcpy(b + at, file.data() + pos, size);
        for (u32 i = 0; i < 8; i++) ps_write32(b, kTracks + 0x40 + 4 * i, (i * 3 + 1) % 5);
        step("cellAtracMultiResetPlayPosition", {kHandle, 0x300, size, kTracks + 0x40});
        step("cellAtracMultiGetTrackArray", {kHandle, kOut + 0x20});
        step("cellAtracMultiResetPlayPosition", {kHandle, 0x300, 1, 0});
    }
    step("cellAtracMultiResetPlayPosition", {kHandle, 0x7FFFFFFF, 0, 0});
    log.push_back(handle_bytes(b));
    step("cellAtracMultiDeleteDecoder", {kHandle});
    step("cellAtracMultiDeleteDecoder", {kHandle});
    log.push_back(handle_bytes(b));
    return log;
}

std::vector<std::string> script(u8* b, const Call& call)
{
    std::vector<std::string> log;
    const auto add = [&](std::string_view what, const std::vector<std::string>& l) {
        log.push_back(std::string(what));
        log.insert(log.end(), l.begin(), l.end());
    };
    const auto plain = at3_file({});
    const u32 size = static_cast<u32>(plain.size());
    add("whole, two tracks", play(b, call, plain, {size, 0x20000}));
    add("whole, tracks picked", play(b, call, plain, {size, 0x20000, 0, false, 4, {3, -1, 1, 9}}));
    add("whole ext, one track", play(b, call, plain, {size, 0x20000, 0, true, 1, {2}}));
    add("half", play(b, call, plain, {0x1000, 0x20000, 0, false, 3, {0, 1, 2}}));
    add("streamed", play(b, call, plain, {0x2000, 0x2400}));
    add("too small", play(b, call, plain, {0x400, 0x2400}));
    const auto fact = at3_file({.frames = 20, .samples = 15000, .offset = 1000});
    add("fact", play(b, call, fact, {0x20000, 0x20000}));
    const auto fact4 = at3_file({.frames = 20, .samples = 15000});
    add("fact 4", play(b, call, fact4, {0x2000, 0x2400}));
    const auto looped = at3_file({.frames = 30, .samples = 25000, .loop_start = 5000, .loop_end = 24999});
    add("loop at end, streamed", play(b, call, looped, {0x2000, 0x2400, 2}));
    add("loop at end, whole", play(b, call, looped, {0x20000, 0x20000, -1}));
    const auto inner = at3_file({.frames = 30, .samples = 27000, .loop_start = 4000, .loop_end = 15000});
    add("inner loop, second buffer", play(b, call, inner, {0x2000, 0x2400, 1}));
    add("inner loop, whole", play(b, call, inner, {0x20000, 0x20000, 3}));
    add("8 tracks", play(b, call, plain, {size, 0x20000, 0, false, 8, {0, 1, 2, 3, 3, 2, 1, 0}}));
    add("9 tracks", play(b, call, plain, {size, 0x20000, 0, false, 9}));
    add("0 tracks", play(b, call, plain, {size, 0x20000, 0, false, 0}));
    add("joint stereo", play(b, call, at3_file({.joint = 1}), {0x20000, 0x20000}));
    add("ATRAC3plus tag", play(b, call, at3_file({.tag = 0xFFFE}), {0x20000, 0x20000}));
    add("32 kHz", play(b, call, at3_file({.rate = 32000}), {0x20000, 0x20000}));
    add("read over buffer", play(b, call, plain, {0x2000, 0x1000}));
    add("12 bytes", play(b, call, plain, {12, 0x1000}));
    // Calls on a handle never set, and NULLs.
    std::memset(b + kHandle, 0, 0x200);
    for (const char* q : {"cellAtracMultiGetChannel", "cellAtracMultiGetRemainFrame", "cellAtracMultiDeleteDecoder",
             "cellAtracMultiGetTrackArray"}) {
        log.push_back(std::format("{} unset -> {:#x}", q, call(q, {kHandle, kOut})));
        log.push_back(std::format("{} NULL -> {:#x}", q, call(q, {0, kOut})));
    }
    log.push_back(std::format("GetChannel to NULL -> {:#x}", call("cellAtracMultiGetChannel", {kHandle, 0})));
    log.push_back(std::format("Decode to NULL -> {:#x}", call("cellAtracMultiDecode", {kHandle, 0, kOut, kOut, kOut})));
    return log;
}

void compare_logs(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
    usize differ = 0;
    for (usize i = 0; i < std::max(a.size(), b.size()); i++) {
        const std::string x = i < a.size() ? a[i] : "(none)", y = i < b.size() ? b[i] : "(none)";
        if (x == y) continue;
        if (++differ <= 30) UNSCOPED_INFO(std::format("{}: {}\n    (pspocket)\n  {}\n    (Sony)", i, x, y));
    }
    INFO(std::format("{} of {} lines differ", differ, b.size()));
    CHECK(differ == 0);
}

struct MultiFixture : Fixture {
    MultiFixture()
    {
        REQUIRE(hle->link_internal_functions({"cellAdec", "cellAtracMulti"}));
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
    }
};

Call ours(MultiFixture& f, GuestThread& t)
{
    return [&f, &t](const char* name, std::initializer_list<u64> args) {
        const Function& fn = GuestThread::find(name);
        u32 r = 3;
        for (const u64 a : args) t.ctx.gpr[r++] = a;
        fn.fn(t.ctx, f.base());
        return static_cast<u32>(t.ctx.gpr[3]);
    };
}

} // namespace

TEST_CASE("cellAtracMulti: ATRAC3 files of four channels built here, as tracks (pspocket)", "[hle][atrac]")
{
    MultiFixture f;
    GuestThread t(f, 0, "ppu");
    const auto log = script(f.base(), ours(f, t));
    CHECK(log.size() > 50); // without the firmware, cellAdec plays it through pspocket's core (D-621)
    CHECK(std::find(log.begin(), log.end(), "cellAtracMultiDeleteDecoder unset -> 0x80610b22") != log.end());
    CHECK(std::find(log.begin(), log.end(), "cellAtracMultiGetChannel NULL -> 0x80610b91") != log.end());
}

TEST_CASE("cellAtracMulti against libatrac3multi: every answer, output, frame and handle byte", "[hle][lle][atrac]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    MultiFixture f;
    GuestThread t(f, 0, "ppu");
    loader::LleModules cores(*flash);
    REQUIRE(cores.index());
    cores.link_host_imports({{"cellAdec", {}, {}}}, *f.hle); // the ATRAC3 multichannel core, as the loader links it
    for (const ModuleEntry& start : cores.take_starts()) run_module_entry(t.ctx, start, 0, 0);
    FirmwareLibrary sony(f, t, *flash, "cellAtracMulti", false);
    const Call theirs = [&](const char* name, std::initializer_list<u64> args) {
        return static_cast<u32>(
            call_guest(t.ctx, sony.module->functions.at({"cellAtracMulti", recomp::nid_of(name)}), args));
    };
    const auto mine = script(f.base(), ours(f, t));
    const auto count = [&](std::string_view prefix) {
        return std::count_if(mine.begin(), mine.end(), [&](const std::string& l) { return l.starts_with(prefix); });
    };
    CHECK(count("cellAtracMultiDecode -> 0x0 ") > 150);
    CHECK(count("cellAtracMultiAddStreamData -> 0x0 ") > 20);
    CHECK(count("cellAtracMultiSetSecondBuffer -> 0x0 ") >= 1);
    CHECK(count("cellAtracMultiResetPlayPosition -> 0x0 ") >= 4);
    compare_logs(mine, script(f.base(), theirs));
}
