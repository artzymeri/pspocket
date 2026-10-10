// cellAtrac (D-532) against libatrac3plus, Sony's own module, from the user's installed system software
// (firmware_compare.hpp; skipped without it): AT3+ files built here (a RIFF WAVE header, frames that decode to silence
// through pspocket's cellAdec on FFmpeg) played whole, half loaded, streamed, looped and with a second buffer; the
// answers, every output, the handle's bytes (but the lv2 IDs, cellAdec's handle and the callback's descriptor) and the
// PCM must be the same. And headers with each thing wrong. No game data.

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

constexpr u32 kHandle = kData + 0x200, kOut = kData + 0x500, kExt = kData + 0x480;
constexpr u32 kFile = kData + 0x10000, kBuffer = kData + 0x60000, kSecond = kData + 0x80000;
constexpr u32 kWork = kData + 0x90000, kPcm = kData + 0xD0000;
constexpr u32 kFrame = 0x2B0; // 48 kHz stereo, 128 kbps

// A valid silent stereo ATRAC3plus frame (as test_adec.cpp builds them).
std::vector<u8> silent_frame(u32 units)
{
    std::vector<u8> f(kFrame, 0);
    u32 bit = 0;
    const auto put = [&](u32 v, u32 n) {
        for (u32 i = 0; i < n; i++, bit++)
            if ((v >> (n - 1 - i)) & 1) f[bit / 8] |= static_cast<u8>(0x80 >> (bit % 8));
    };
    put(0, 1);
    put(1, 2);
    put(units - 1, 5);
    put(0, 1);
    for (u32 ch = 0; ch < 2; ch++) {
        put(0, 2);
        for (u32 q = 0; q < units; q++) put(0, 3);
    }
    put(0, 2);
    put(0, 2);
    put(0, 2);
    put(0, 1);
    put(0, 1);
    put(3, 2);
    return f;
}

struct FileSpec {
    u32 frames = 24;
    s32 samples = -1;     // the fact chunk's samples (-1: no fact)
    s32 offset = -1;      // the fact chunk's first sample offset (-1: a 4-byte fact)
    s32 loop_start = -1;  // smpl's loop (-1: none)
    s32 loop_end = -1;
    u32 rate = 48000;
    u16 tag = 0xFFFE;
    bool bad_guid = false;
    bool odd_chunk = false; // an unknown chunk of odd size before data
};

// An AT3+ file: RIFF WAVE, fmt (WAVE_FORMAT_EXTENSIBLE with ATRAC3plus' GUID and config: 48 kHz stereo, 0x2B0-byte
// frames), fact, smpl, an optional unknown chunk, data.
std::vector<u8> at3p_file(const FileSpec& s)
{
    std::vector<u8> f;
    const auto u16le = [&](u32 v) { f.push_back(static_cast<u8>(v)); f.push_back(static_cast<u8>(v >> 8)); };
    const auto u32le = [&](u32 v) { u16le(v & 0xFFFF); u16le(v >> 16); };
    const auto tag = [&](const char* t) { f.insert(f.end(), t, t + 4); };
    tag("RIFF");
    u32le(0);
    tag("WAVE");
    tag("fmt ");
    u32le(0x34);
    u16le(s.tag);
    u16le(2);
    u32le(s.rate);
    u32le(s.rate * kFrame / 2048);
    u16le(kFrame);
    u16le(0);
    u16le(0x22);
    u16le(0x800);
    u32le(3);
    static const u8 kGuid[16] = {0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44,
                                 0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62};
    f.insert(f.end(), kGuid, kGuid + 16);
    if (s.bad_guid) f[f.size() - 3] ^= 1;
    u16le(1);
    f.push_back(0x48); // 48 kHz (2 << 5), stereo (2 << 2)
    f.push_back(0x55); // 0x2B0 / 8 - 1
    for (u32 i = 0; i < 8; i++) f.push_back(0);
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
    if (s.odd_chunk) {
        tag("LIST");
        u32le(5);
        for (u32 i = 0; i < 6; i++) f.push_back(0x20);
    }
    tag("data");
    u32le(s.frames * kFrame);
    for (u32 i = 0; i < s.frames; i++) {
        const auto fr = silent_frame(1 + (i * 7) % 28);
        f.insert(f.end(), fr.begin(), fr.end());
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

// One play-through of `file` read `first` bytes at a time into a buffer of `buffer_size`, the stream topped up as the
// library asks, with `loops` set and a reset at the end; the log of everything it answers.
std::vector<std::string> play(u8* b, const Call& call, const std::vector<u8>& file, u32 first, u32 buffer_size, s32 loops,
    bool ext)
{
    std::vector<std::string> log;
    std::memset(b + kHandle, 0, 0x200);
    std::memset(b + kBuffer, 0xEE, 0x20000);
    std::memset(b + kOut, 0, 0x40);
    std::memcpy(b + kFile, file.data(), file.size());
    const u32 read = std::min<u32>(first, static_cast<u32>(file.size()));
    std::memcpy(b + kBuffer, file.data(), read);
    const auto step = [&](const char* name, std::initializer_list<u64> args) {
        const u32 r = call(name, args);
        std::string line = std::format("{} -> {:#x}", name, r);
        for (u32 i = 0; i < 8; i++) line += std::format(" {:x}", ps_read32(b, kOut + 4 * i));
        log.push_back(line);
        return r;
    };
    if (step("cellAtracSetDataAndGetMemSize", {kHandle, kBuffer, read, buffer_size, kOut}) != 0) {
        log.push_back(handle_bytes(b));
        return log;
    }
    log.push_back(handle_bytes(b));
    if (ext) {
        ps_write32(b, kExt, 0);
        for (u32 i = 0; i < 8; i++) b[kExt + 4 + i] = static_cast<u8>(i + 1);
        step("cellAtracCreateDecoderExt", {kHandle, kWork, 1000, kExt});
    } else {
        step("cellAtracCreateDecoder", {kHandle, kWork, 1000, 100});
    }
    log.push_back(handle_bytes(b));
    for (const char* q : {"cellAtracGetChannel", "cellAtracGetMaxSample", "cellAtracGetSamplingRate", "cellAtracGetBitrate",
             "cellAtracGetNextSample", "cellAtracGetRemainFrame", "cellAtracGetVacantSize", "cellAtracGetNextDecodePosition",
             "cellAtracGetInternalErrorInfo"})
        step(q, {kHandle, kOut});
    step("cellAtracGetSoundInfo", {kHandle, kOut, kOut + 4, kOut + 8});
    step("cellAtracIsSecondBufferNeeded", {kHandle});
    step("cellAtracGetSecondBufferInfo", {kHandle, kOut, kOut + 4});
    if (ps_read32(b, kOut + 4)) {
        const u32 from = ps_read32(b, kOut), size = ps_read32(b, kOut + 4);
        std::memcpy(b + kSecond, file.data() + from, std::min<u32>(size, static_cast<u32>(file.size()) - from));
        step("cellAtracSetSecondBuffer", {kHandle, kSecond, 0x100});
        step("cellAtracSetSecondBuffer", {kHandle, kSecond, size});
    }
    step("cellAtracSetLoopNum", {kHandle, static_cast<u32>(loops)});
    step("cellAtracGetLoopInfo", {kHandle, kOut, kOut + 4});
    for (u32 n = 0; n < 200; n++) {
        step("cellAtracGetStreamDataInfo", {kHandle, kOut + 0x10, kOut + 0x14, kOut + 0x18});
        const u32 write = ps_read32(b, kOut + 0x10), size = ps_read32(b, kOut + 0x14), pos = ps_read32(b, kOut + 0x18);
        if (size && pos < file.size() && write >= kBuffer && write < kBuffer + 0x20000 && (n < 12 || n % 3 != 2)) {
            const u32 take = std::min({size, static_cast<u32>(file.size()) - pos, n < 12 ? 3 * kFrame + n % 5 * 0x40 : kFrame});
            std::memcpy(b + write, file.data() + pos, take);
            step("cellAtracAddStreamData", {kHandle, take});
        }
        std::memset(b + kPcm, 0xCC, 0x4000);
        const u32 r = step("cellAtracDecode", {kHandle, kPcm, kOut, kOut + 4, kOut + 8});
        std::string pcm = "pcm";
        for (u32 i = 0; i < 0x4000; i += 0x400) pcm += std::format(" {:x}", ps_read32(b, kPcm + i));
        log.push_back(pcm);
        step("cellAtracGetVacantSize", {kHandle, kOut + 0xC});
        step("cellAtracGetNextDecodePosition", {kHandle, kOut + 0xC});
        log.push_back(handle_bytes(b));
        if ((r != 0 && r != 0x80610332) || ps_read32(b, kOut + 4)) break;
    }
    std::memset(b + kOut + 0x10, 0, 0x10);
    const u32 info = step("cellAtracGetBufferInfoForResetting", {kHandle, 0x300, kOut + 0x10});
    const u32 at = ps_read32(b, kOut + 0x10), pos = ps_read32(b, kOut + 0x1C);
    if (info == 0 && ps_read32(b, kOut + 0x14) && pos < file.size() && at >= kBuffer && at < kBuffer + 0x20000) {
        const u32 size = std::min<u32>(ps_read32(b, kOut + 0x14), static_cast<u32>(file.size()) - pos);
        std::memcpy(b + at, file.data() + pos, size);
        step("cellAtracResetPlayPosition", {kHandle, 0x300, size});
        step("cellAtracResetPlayPosition", {kHandle, 0x300, 1});
    }
    step("cellAtracResetPlayPosition", {kHandle, 0x7FFFFFFF, 0});
    log.push_back(handle_bytes(b));
    step("cellAtracDeleteDecoder", {kHandle});
    step("cellAtracDeleteDecoder", {kHandle});
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
    const auto plain = at3p_file({});
    const u32 size = static_cast<u32>(plain.size());
    add("whole", play(b, call, plain, size, 0x20000, 0, false));
    add("whole ext", play(b, call, plain, size, 0x20000, 0, true));
    add("half", play(b, call, plain, 0x1000, 0x20000, 0, false));
    add("streamed", play(b, call, plain, 0x1400, 0x1800, 0, false));
    add("too small", play(b, call, plain, 0x400, 0x1800, 0, false));
    const auto fact = at3p_file({.frames = 20, .samples = 30000, .offset = 1000});
    add("fact", play(b, call, fact, 0x20000, 0x20000, 0, false));
    const auto fact4 = at3p_file({.frames = 20, .samples = 30000});
    add("fact 4", play(b, call, fact4, 0x1400, 0x1800, 0, false));
    const auto looped = at3p_file({.frames = 30, .samples = 50000, .loop_start = 10000, .loop_end = 49999});
    add("loop at end, streamed", play(b, call, looped, 0x1400, 0x1800, 2, false));
    add("loop at end, whole", play(b, call, looped, 0x20000, 0x20000, -1, false));
    const auto inner = at3p_file({.frames = 30, .samples = 55000, .loop_start = 8000, .loop_end = 30000});
    add("inner loop, second buffer", play(b, call, inner, 0x1400, 0x1800, 1, false));
    add("inner loop, whole", play(b, call, inner, 0x20000, 0x20000, 3, false));
    add("odd chunk", play(b, call, at3p_file({.odd_chunk = true}), 0x20000, 0x20000, 0, false));
    add("44.1 kHz", play(b, call, at3p_file({.rate = 44100}), 0x20000, 0x20000, 0, false));
    add("32 kHz", play(b, call, at3p_file({.rate = 32000}), 0x20000, 0x20000, 0, false));
    add("wrong GUID", play(b, call, at3p_file({.bad_guid = true}), 0x20000, 0x20000, 0, false));
    add("PCM tag", play(b, call, at3p_file({.tag = 1}), 0x20000, 0x20000, 0, false));
    add("read over buffer", play(b, call, plain, 0x2000, 0x1000, 0, false));
    add("12 bytes", play(b, call, plain, 12, 0x1000, 0, false));
    // Calls on a handle never set.
    std::memset(b + kHandle, 0, 0x200);
    for (const char* q : {"cellAtracGetChannel", "cellAtracGetRemainFrame", "cellAtracDeleteDecoder"})
        log.push_back(std::format("{} unset -> {:#x}", q, call(q, {kHandle, kOut})));
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

} // namespace

TEST_CASE("cellAtrac: AT3+ files built here, whole, streamed and looped (pspocket)", "[hle][atrac]")
{
#if !PSPOCKET_HAVE_FFMPEG
    SKIP("no FFmpeg in this build (scripts/build-ffmpeg.sh): pspocket's cellAdec has no ATRAC3plus core");
#endif
    Fixture f;
    REQUIRE(f.hle->link_internal_functions({"cellAdec", "cellAtrac"}));
    GuestThread t(f, 0, "ppu");
    const Call call = [&](const char* name, std::initializer_list<u64> args) {
        const Function& fn = GuestThread::find(name);
        u32 r = 3;
        for (const u64 a : args) t.ctx.gpr[r++] = a;
        fn.fn(t.ctx, f.base());
        return static_cast<u32>(t.ctx.gpr[3]);
    };
    const auto log = script(f.base(), call);
    CHECK(log.size() > 200);
    CHECK(std::find(log.begin(), log.end(), "cellAtracDeleteDecoder unset -> 0x80610322") != log.end());
}

TEST_CASE("cellAtrac against libatrac3plus: every answer, output and handle byte", "[hle][lle][atrac]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    Fixture f;
    REQUIRE(f.hle->link_internal_functions({"cellAdec", "cellAtrac"}));
    GuestThread t(f, 0, "ppu");
    t.call("sys_initialize_tls", 0, 0, 0, 0);
    FirmwareLibrary sony(f, t, *flash, "cellAtrac", false);
    const Call mine = [&](const char* name, std::initializer_list<u64> args) {
        const Function& fn = GuestThread::find(name);
        u32 r = 3;
        for (const u64 a : args) t.ctx.gpr[r++] = a;
        fn.fn(t.ctx, f.base());
        return static_cast<u32>(t.ctx.gpr[3]);
    };
    const Call theirs = [&](const char* name, std::initializer_list<u64> args) {
        return static_cast<u32>(call_guest(t.ctx, sony.module->functions.at({"cellAtrac", recomp::nid_of(name)}), args));
    };
    const auto ours = script(f.base(), mine);
    const auto count = [&](std::string_view prefix) {
        return std::count_if(ours.begin(), ours.end(), [&](const std::string& l) { return l.starts_with(prefix); });
    };
    CHECK(count("cellAtracDecode -> 0x0 ") > 150);
    CHECK(count("cellAtracAddStreamData -> 0x0 ") > 20);
    CHECK(count("cellAtracSetSecondBuffer -> 0x0 ") >= 1);
    CHECK(count("cellAtracResetPlayPosition -> 0x0 ") >= 4);
    CHECK(count("cellAtracDecode -> 0x80610332") >= 1); // a stream that ran dry
    compare_logs(ours, script(f.base(), theirs));
}
