// cellAdec (D-197): pspocket's audio decoder front end and LPCM core, alone and against Sony's libadec from the user's
// installed system software (PSPOCKET_TEST_DEV_FLASH, firmware_compare.hpp). The audio is built here (no game data):
// tones as 16- and 24-bit LPCM in the PAMF, Blu-ray and DVD packings. The same calls go to both decoders; every answer,
// callback, PCM item, bitstream information block and output sample is compared.

#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "adec_test_util.hpp"
#include "firmware_compare.hpp"
#include "hle_test_util.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/ppu/asm.hpp"
#include "pspocket/spu/runtime.h"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::adec_test;

namespace {

// `samples` samples of `channels` interleaved channels, `bytes` (2 or 3) big-endian bytes each: a different tone in
// each channel, so a channel in the wrong place shows.
std::vector<u8> tones(u32 samples, u32 channels, u32 bytes, u32 seed)
{
    std::vector<u8> au;
    for (u32 i = 0; i < samples; i++) {
        for (u32 ch = 0; ch < channels; ch++) {
            const double x = std::sin(0.013 * (i + seed * samples) * (ch + 1) + ch) * (ch + 2) / (channels + 2);
            const auto v = static_cast<s32>(x * (bytes == 2 ? 32000.0 : 8000000.0)) + static_cast<s32>(ch);
            for (u32 k = 0; k < bytes; k++) au.push_back(static_cast<u8>(static_cast<u32>(v) >> (8 * (bytes - 1 - k))));
        }
    }
    return au;
}

// `n` bytes of a pattern (samples in DVD's 20- and 24-bit packing, where any bytes are samples).
std::vector<u8> bytes(u32 n, u32 seed)
{
    std::vector<u8> v(n);
    u32 x = 0x9E3779B9u * (seed + 1);
    for (u8& c : v) {
        x = x * 1664525u + 1013904223u;
        c = static_cast<u8>(x >> 24);
    }
    return v;
}


// One sequence: the parameters (CellAdecParamLpcm, or 0 for the Blu-ray and DVD types, whose parameters come with each
// access unit) and its access units (bytes, user data).
struct Sequence {
    u32 param[4];
    std::vector<std::pair<std::vector<u8>, u64>> aus;
};

Observed adec_run(u8* b, const Call& call)
{
    Observed o;
    const auto r = [&](const char* name, u32 nid, std::initializer_list<u64> args) {
        const u32 v = call(name, nid, args);
        o.lines.push_back(std::format("{} -> {:#x}", name, v));
        return v;
    };
    const auto note = [&](std::string s) { o.lines.push_back(std::move(s)); };
    std::memset(b + kArgs, 0, 0x200);

    // Sizes for the three LPCM types; refusals.
    for (const u32 type : {1u, 12u, 17u}) {
        ps_write32(b, kType, type);
        std::memset(b + kAttr, 0xEE, 12);
        r("cellAdecQueryAttr", 0x7E4A4A49, {kType, kAttr});
        note(std::format("attr {}: {:#x} {:#x} {:#x}", type, ps_read32(b, kAttr), ps_read32(b, kAttr + 4),
            ps_read32(b, kAttr + 8)));
    }
    for (const u32 type : {0u, 26u, 27u, 32u, 0xFFFFFFFFu}) {
        ps_write32(b, kType, type);
        r("cellAdecQueryAttr", 0x7E4A4A49, {kType, kAttr});
    }
    r("cellAdecQueryAttr", 0x7E4A4A49, {0, kAttr});
    ps_write32(b, kType, 1);
    r("cellAdecQueryAttr", 0x7E4A4A49, {kType, 0});

    // A decoder of `type` through its sequences; each access unit decoded and its PCM taken before the next, except
    // where `queue` asks for several at once.
    const auto decoder = [&](u32 type, const std::vector<Sequence>& seqs, bool queue) {
        note(std::format("decoder of type {}", type));
        write_callback(b);
        ps_write32(b, kType, type);
        r("cellAdecQueryAttr", 0x7E4A4A49, {kType, kAttr});
        const u32 mem = ps_read32(b, kAttr);
        // CellAdecResource: totalMemSize, startAddr, ppuThreadPriority, spuThreadPriority, ppuThreadStackSize.
        const u32 res[5] = {mem, kDecoderMem, 1000, 200, 0x800};
        for (u32 i = 0; i < 5; i++) ps_write32(b, kRes + 4 * i, res[i]);
        ps_write32(b, kCb, kOpd);
        ps_write32(b, kCb + 4, kLog);
        r("cellAdecOpen", 0xD00A6988, {kType, kRes, kCb, kHandle}); // the stack is too small
        ps_write32(b, kRes + 16, 0x4000);
        ps_write32(b, kRes, mem - 1);
        r("cellAdecOpen", 0xD00A6988, {kType, kRes, kCb, kHandle}); // the memory is too small
        ps_write32(b, kRes, mem);
        if (r("cellAdecOpen", 0xD00A6988, {kType, kRes, kCb, kHandle}) != 0) return;
        const u32 handle = ps_read32(b, kHandle);
        note(std::format("handle {:#x} (memory {:+#x})", handle, static_cast<s64>(handle) - kDecoderMem));
        // Before a sequence: nothing out, nothing to end or decode.
        r("cellAdecGetPcmItem", 0xBD75F78B, {handle, kItemOut});
        r("cellAdecGetPcm", 0x97FF2AF1, {handle, kPcm});
        r("cellAdecEndSeq", 0xE2EA549B, {handle});
        ps_write32(b, kAu + 0, kStream);
        ps_write32(b, kAu + 4, 0x100);
        r("cellAdecDecodeAu", 0x1529E506, {handle, kAu});
        u32 done = 0, au_index = 0, seq_index = 0;
        // The PCM out next: its item, information and samples.
        const auto take = [&](bool with_item) {
            u32 item = 0;
            if (with_item) {
                ps_write32(b, kItemOut, 0);
                if (r("cellAdecGetPcmItem", 0xBD75F78B, {handle, kItemOut}) != 0) return;
                item = ps_read32(b, kItemOut);
            }
            if (item) {
                note(std::format("item at {:+#x}: {}", static_cast<s64>(item) - handle, hex(b + item, 0x30)));
                const u32 bsi = ps_read32(b, item + 0x10);
                note(std::format("  information at {:+#x}: {}", static_cast<s64>(bsi) - handle, hex(b + bsi, 12)));
            }
            const u32 size = item ? ps_read32(b, item + 0xC) : 0;
            std::memset(b + kPcm, 0xCC, 0x8000);
            if (r("cellAdecGetPcm", 0x97FF2AF1, {handle, kPcm}) != 0) return;
            u32 written = 0x8000;
            while (written && b[kPcm + written - 1] == 0xCC) written--;
            note(std::format("  pcm: {} bytes written{}", written, item ? std::format(" (item size {})", size) : ""));
            o.pcm.emplace_back(b + kPcm, b + kPcm + written);
        };
        const auto put_au = [&](const std::vector<u8>& au, u64 user_data, bool pts) {
            const u32 at = kStream + 0x2000 * (au_index % 32);
            std::memcpy(b + at, au.data(), au.size());
            // CellAdecAuInfo: startAddr, size, pts (upper, lower), userData. Some without a PTS (the front end's
            // correction).
            ps_write32(b, kAu + 0, at);
            ps_write32(b, kAu + 4, static_cast<u32>(au.size()));
            ps_write32(b, kAu + 8, pts ? 0 : 0xFFFFFFFF);
            ps_write32(b, kAu + 12, pts ? 0x1000 + 1920 * au_index : 0xFFFFFFFF);
            ps_write64(b, kAu + 16, user_data);
            au_index++;
            return r("cellAdecDecodeAu", 0x1529E506, {handle, kAu});
        };
        for (const Sequence& s : seqs) {
            for (u32 i = 0; i < 4; i++) ps_write32(b, kParam + 4 * i, s.param[i]);
            if (seq_index++ == 0) {
                const u32 payload = ps_read32(b, kParam + 12);
                ps_write32(b, kParam + 12, 0);
                r("cellAdecStartSeq", 0x487B613E, {handle, kParam}); // no payload
                ps_write32(b, kParam + 12, payload);
            }
            if (r("cellAdecStartSeq", 0x487B613E, {handle, kParam}) != 0) return;
            for (usize i = 0; i < s.aus.size(); i++) {
                if (put_au(s.aus[i].first, s.aus[i].second, i % 3 != 1) != 0) continue;
                done++;
                if (!wait_until([&] { return count_of(b, 1) >= done; })) note("no PCMOUT");
                take(i % 2 == 0);
            }
            if (queue && seq_index == 1) {
                // Several at once: the first's output waits to be taken; the commands behind it queue (four), the
                // next is BUSY.
                put_au(s.aus[0].first, s.aus[0].second, true);
                if (!wait_until([&] { return count_of(b, 1) >= done + 1; })) note("no PCMOUT");
                for (u32 k = 0; k < 5; k++) put_au(s.aus[0].first, s.aus[0].second, true);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                note(std::format("out while the first is not taken: {}", count_of(b, 1) - done));
                for (u32 k = 0; k < 5; k++) {
                    done++;
                    if (!wait_until([&] { return count_of(b, 1) >= done; })) note("no PCMOUT");
                    take(k % 2 == 1);
                }
            }
        }
        r("cellAdecEndSeq", 0xE2EA549B, {handle});
        if (!wait_until([&] { return count_of(b, 3) >= 1; })) note("no SEQDONE");
        r("cellAdecGetPcm", 0x97FF2AF1, {handle, kPcm});
        for (u32 i = 0; i < log_count(b); i++) {
            const u32 type_i = ps_read32(b, kLog + 16 + 16 * i), data = ps_read32(b, kLog + 20 + 16 * i);
            note(std::format("callback {}: type {} data {} handle {:+#x}", i, type_i,
                type_i == 0 ? std::format("{:+#x}", static_cast<s64>(data) - handle) : std::format("{:#x}", data),
                static_cast<s64>(ps_read32(b, kLog + 24 + 16 * i)) - handle));
        }
        r("cellAdecClose", 0x847D2380, {handle});
        r("cellAdecClose", 0x847D2380, {handle});                   // closed
        r("cellAdecStartSeq", 0x487B613E, {handle, kParam});        // closed
    };

    // PAMF: 16-bit stereo (four sequences' worth of access units), mono, 3/2 (five channels and the dummy) at 24 bits,
    // 3/4 with LFE; then the commands queued behind an output not taken.
    std::vector<Sequence> pamf;
    pamf.push_back({{3, 1, 1, 1024 * 4}, {}});
    for (u32 i = 0; i < 4; i++) pamf.back().aus.push_back({tones(1024, 2, 2, i), 0});
    pamf.push_back({{1, 1, 1, 640 * 2 * 2}, {{tones(640, 2, 2, 7), 0}}}); // mono carries a dummy channel
    pamf.push_back({{8, 1, 3, 256 * 6 * 3}, {{tones(256, 6, 3, 9), 0}}});
    pamf.push_back({{11, 1, 1, 160 * 8 * 2}, {{tones(160, 8, 2, 11), 0}}});
    pamf.push_back({{4, 1, 1, 200 * 4 * 2}, {{tones(200, 4, 2, 13), 0}}}); // 3/0 with a dummy
    decoder(1, pamf, true);
    // Blu-ray: the parameters in each access unit's user data (channels << 32, rate << 40, word << 48, frames << 56).
    const auto bd = [](u64 ch, u64 rate, u64 word, u64 frames) { return ch << 32 | rate << 40 | word << 48 | frames << 56; };
    // The third's channel layout is reserved: FATAL.
    decoder(12, {{{3, 1, 1, 0x780}, {{tones(240, 2, 2, 1), bd(3, 1, 1, 1)}, {tones(240, 6, 3, 2), bd(9, 1, 3, 1)},
                    {tones(240, 2, 2, 3), bd(2, 1, 1, 1)}, {tones(240, 4, 2, 4), bd(6, 1, 1, 1)}}}},
        false);
    // DVD: 16-bit stereo, 16-bit 5.1 (only the front pair decoded), 20 and 24 bits in DVD's packing (bytes as they
    // come), 24-bit 5.0; 5.1 at 24 bits is refused.
    decoder(17, {{{3, 1, 1, 0x140}, {{tones(80, 2, 2, 1), bd(3, 1, 1, 1)}, {tones(80, 6, 2, 2), bd(7, 1, 1, 1)},
                    {bytes(400, 3), bd(3, 1, 2, 1)}, {bytes(480, 4), bd(3, 1, 3, 1)}, {bytes(1200, 5), bd(6, 1, 3, 1)},
                    {bytes(1200, 6), bd(5, 1, 2, 1)}, {bytes(1440, 7), bd(7, 1, 3, 1)}}}},
        false);
    return o;
}

// An MPEG-1 layer I frame (48 kHz, 384 kbit/s, stereo, no CRC: 384 bytes) whose allocations, scale factors and samples
// are a pattern: any such frame decodes to sound.
std::vector<u8> layer1_frame(u32 seed)
{
    std::vector<u8> f(384, 0);
    u32 bit = 0;
    const auto put = [&](u32 v, u32 n) {
        for (u32 i = 0; i < n; i++, bit++)
            if ((v >> (n - 1 - i)) & 1) f[bit / 8] |= static_cast<u8>(0x80 >> (bit % 8));
    };
    put(0xFFFFC400, 32); // sync, MPEG-1, layer I, no CRC, 384 kbit/s, 48 kHz, stereo
    u32 alloc[32][2];
    for (u32 sb = 0; sb < 32; sb++)
        for (u32 ch = 0; ch < 2; ch++) alloc[sb][ch] = sb < 16 ? (sb + ch + seed) % 4 + 1 : 0;
    for (u32 sb = 0; sb < 32; sb++)
        for (u32 ch = 0; ch < 2; ch++) put(alloc[sb][ch], 4);
    for (u32 sb = 0; sb < 32; sb++)
        for (u32 ch = 0; ch < 2; ch++)
            if (alloc[sb][ch]) put((sb * 3 + ch + seed) % 40 + 5, 6);
    u32 x = 0x2545F491u + seed;
    for (u32 s = 0; s < 12; s++)
        for (u32 sb = 0; sb < 32; sb++)
            for (u32 ch = 0; ch < 2; ch++) {
                if (!alloc[sb][ch]) continue;
                x = x * 1664525u + 1013904223u;
                const u32 nb = alloc[sb][ch] + 1;
                put((x >> 8) % ((1u << nb) - 1), nb); // never all ones
            }
    return f;
}

} // namespace

TEST_CASE("cellAdec: LPCM decoded as Sony's libadec does it (D-197)", "[hle][adec]")
{
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    const Observed o = adec_run(f.base(), hle_calls(f, t));
    INFO(all_lines(o.lines));
    const auto line = [&](std::string_view prefix) {
        const std::string* l = find_line(o.lines, prefix);
        return l ? *l : std::string("(missing)");
    };
    // Sony's answers (the user's 4.92 libadec, tests below).
    CHECK(line("attr 1:") == "attr 1: 0x7f524 0x492000 0x20070323");
    CHECK(line("handle ") == "handle 0x1000000 (memory +0x0)");
    CHECK(line("item at ").starts_with("item at +0x558: 00000000"));
    CHECK(line("  information at ") == "  information at +0x870: 000000030000000100002000");
    CHECK(line("callback 0:") == "callback 0: type 0 data +0x540 handle +0x0");
    // The first PCM: stereo 16-bit samples as floats, sample / 32768.
    REQUIRE(!o.pcm.empty());
    const std::vector<u8> au = tones(1024, 2, 2, 0);
    REQUIRE(o.pcm[0].size() == 1024 * 2 * 4);
    for (usize i = 0; i < 2048; i++) {
        const auto s = static_cast<s16>(static_cast<u16>(au[2 * i] << 8 | au[2 * i + 1]));
        if (be_float(o.pcm[0], i) != static_cast<float>(s) / 32768.f) FAIL("sample " << i);
    }
    CHECK(line("out while the first is not taken:") == "out while the first is not taken: 1");
    CHECK(o.lines.back() == "cellAdecStartSeq -> 0x80610002");
}

TEST_CASE("cellAdec: the same as Sony's libadec (the user's firmware): answers, callbacks, items, PCM", "[hle][adec][firmware]")
{
    const auto flash = test_dev_flash();
    if (!flash) {
        SUCCEED("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
        return;
    }
    InterpretMisses interpret;
    Observed ours, theirs;
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        ours = adec_run(f.base(), hle_calls(f, t));
    }
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        FirmwareLibrary sony(f, t, *flash, "cellAdec");
        theirs = adec_run(f.base(),
            [&](const char*, u32 nid, std::initializer_list<u64> args) { return sony.call(t, nid, args); });
    }
    usize differ = 0;
    for (usize i = 0; i < std::max(ours.lines.size(), theirs.lines.size()); i++) {
        const std::string x = i < ours.lines.size() ? ours.lines[i] : "(none)";
        const std::string y = i < theirs.lines.size() ? theirs.lines[i] : "(none)";
        if (x == y) continue;
        if (++differ <= 40) UNSCOPED_INFO(std::format("line {}:\n  pspocket: {}\n  Sony:     {}", i, x, y));
    }
    REQUIRE(ours.pcm.size() == theirs.pcm.size());
    for (usize i = 0; i < ours.pcm.size(); i++) {
        if (ours.pcm[i] == theirs.pcm[i]) continue;
        usize first = 0;
        while (first < std::min(ours.pcm[i].size(), theirs.pcm[i].size()) && ours.pcm[i][first] == theirs.pcm[i][first])
            first++;
        if (++differ <= 60)
            UNSCOPED_INFO(std::format("pcm {} differs ({} and {} bytes), from byte {}", i, ours.pcm[i].size(),
                theirs.pcm[i].size(), first));
    }
    usize bytes = 0;
    for (const auto& p : theirs.pcm) bytes += p.size();
    INFO(std::format("{} differences in {} lines and {} PCM", differ, ours.lines.size(), ours.pcm.size()));
    CHECK(differ == 0);
    WARN(std::format("compared with Sony's libadec: {} lines, {} PCM outputs ({} bytes)", theirs.lines.size(),
        theirs.pcm.size(), bytes));
    if (const char* dump = std::getenv("PSPOCKET_PROBE_DUMP")) {
        if (FILE* fp = std::fopen(dump, "w")) {
            for (usize i = 0; i < theirs.lines.size(); i++)
                std::fprintf(fp, "%s\n%s\n", theirs.lines[i].c_str(), i < ours.lines.size() ? ours.lines[i].c_str() : "-");
            std::fclose(fp);
        }
    }
}

TEST_CASE("cellAdec: a firmware core (MPEG layer I) through pspocket's front end, as through Sony's libadec",
    "[hle][adec][firmware]")
{
    const auto flash = test_dev_flash();
    if (!flash) {
        SUCCEED("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
        return;
    }
    InterpretMisses interpret;
    std::vector<std::vector<u8>> aus;
    for (u32 k = 0; k < 4; k++) aus.push_back(layer1_frame(k));
    constexpr u32 kMpegL1 = 22;
    Observed ours, theirs;
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        FirmwareCores cores(f, t, *flash);
        REQUIRE(f.hle->imported("cellMPL1dec", 0xBBC70551) != 0);
        ours = core_run(f.base(), hle_calls(f, t), kMpegL1, {3, 0, 0}, aus);
    }
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        FirmwareLibrary sony(f, t, *flash, "cellAdec");
        theirs = core_run(f.base(),
            [&](const char*, u32 nid, std::initializer_list<u64> args) { return sony.call(t, nid, args); }, kMpegL1,
            {3, 0, 0}, aus);
    }
    INFO(all_lines(theirs.lines));
    const usize differ = compare(ours, theirs);
    CHECK(differ == 0);
    CHECK(theirs.pcm.size() >= 3);
    usize loud = 0;
    for (const auto& p : theirs.pcm)
        for (usize i = 0; i + 4 <= p.size(); i += 4) loud += std::abs(be_float(p, i / 4)) > 0.001f;
    CHECK(loud > 1000); // sound, not silence
    WARN(std::format("MPEG layer I through Sony's core: {} lines, {} PCM outputs the same; {} samples above -60 dB",
        theirs.lines.size(), theirs.pcm.size(), loud));
}

// ATRAC3plus, pspocket's core on FFmpeg against Sony's (libatxdec through Sony's libadec), on a stream from a RIFF .at3
// file named by PSPOCKET_PROBE_AT3 (a game's SND0.AT3, say: game data, never in the repository; run 105). The answers,
// items, information and callbacks are compared exactly; the samples by their difference.
TEST_CASE("cellAdec: ATRAC3plus on FFmpeg against Sony's core (probe)", "[hle][adec][firmware][.probe]")
{
    const auto flash = test_dev_flash();
    const char* path = std::getenv("PSPOCKET_PROBE_AT3");
    if (!flash || !path) return;
    std::vector<u8> file;
    {
        FILE* fp = std::fopen(path, "rb");
        REQUIRE(fp);
        u8 buf[4096];
        for (usize n; (n = std::fread(buf, 1, sizeof(buf), fp)) > 0;) file.insert(file.end(), buf, buf + n);
        std::fclose(fp);
    }
    // RIFF WAVE: fmt (channels +2, rate +4, block_align +12; the ATRAC3plus config word big-endian at +0x2A), data.
    REQUIRE(file.size() > 12);
    u32 channels = 0, rate = 0, block = 0, data = 0, data_size = 0;
    u8 config[4] = {};
    const auto le32 = [&](usize at) { return u32{file[at]} | u32{file[at + 1]} << 8 | u32{file[at + 2]} << 16 | u32{file[at + 3]} << 24; };
    for (usize off = 12; off + 8 <= file.size();) {
        const u32 size = le32(off + 4);
        if (std::memcmp(&file[off], "fmt ", 4) == 0) {
            channels = file[off + 10] | file[off + 11] << 8;
            rate = le32(off + 12);
            block = file[off + 20] | file[off + 21] << 8;
            std::memcpy(config, &file[off + 8 + 0x28], 4);
        }
        if (std::memcmp(&file[off], "data", 4) == 0) {
            data = static_cast<u32>(off + 8);
            data_size = size;
            break;
        }
        off += 8 + size + (size & 1);
    }
    REQUIRE((data && block && channels));
    std::vector<std::vector<u8>> aus;
    for (u32 at = data; at + block <= data + data_size && aus.size() < 1024; at += block)
        aus.emplace_back(file.begin() + at, file.begin() + at + block);
    const u32 ch_config = channels <= 4 ? channels : channels - 1;
    // CellAdecParamAtracX: sampling_freq, ch_config_idx, nch_out, nbytes, extra_config_data, bw_pcm (float), downmix,
    // ATS headers (none).
    const u32 extra = u32{config[0]} << 24 | u32{config[1]} << 16 | u32{config[2]} << 8 | config[3];
    const std::initializer_list<u32> param = {rate, ch_config, channels, block, extra, 0x84, 0};
    InterpretMisses interpret;
    Observed ours, theirs;
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        ours = core_run(f.base(), hle_calls(f, t), 3, param, aus);
    }
    const auto t1 = clock::now();
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        FirmwareLibrary sony(f, t, *flash, "cellAdec");
        theirs = core_run(f.base(),
            [&](const char*, u32 nid, std::initializer_list<u64> args) { return sony.call(t, nid, args); }, 3, param, aus);
    }
    const auto t2 = clock::now();
    if (const char* dump = std::getenv("PSPOCKET_PROBE_DUMP")) {
        if (FILE* fp = std::fopen(dump, "w")) {
            for (usize i = 0; i < std::max(ours.lines.size(), theirs.lines.size()); i++)
                std::fprintf(fp, "S %s\nP %s\n", i < theirs.lines.size() ? theirs.lines[i].c_str() : "-",
                    i < ours.lines.size() ? ours.lines[i].c_str() : "-");
            std::fclose(fp);
        }
    }
    usize differ = 0;
    for (usize i = 0; i < std::max(ours.lines.size(), theirs.lines.size()); i++) {
        const std::string x = i < ours.lines.size() ? ours.lines[i] : "(none)";
        const std::string y = i < theirs.lines.size() ? theirs.lines[i] : "(none)";
        if (x != y && ++differ <= 30) UNSCOPED_INFO(std::format("line {}:\n  pspocket: {}\n  Sony:     {}", i, x, y));
    }
    double signal = 0, noise = 0, max_diff = 0;
    usize samples = 0;
    for (usize i = 0; i < std::min(ours.pcm.size(), theirs.pcm.size()); i++) {
        const usize n = std::min(ours.pcm[i].size(), theirs.pcm[i].size()) / 4;
        for (usize k = 0; k < n; k++) {
            const double a = static_cast<double>(be_float(ours.pcm[i], k)), c = static_cast<double>(be_float(theirs.pcm[i], k));
            signal += c * c;
            noise += (a - c) * (a - c);
            max_diff = std::max(max_diff, std::abs(a - c));
            samples++;
        }
    }
    const double snr = noise > 0 ? 10 * std::log10(signal / noise) : 999;
    WARN(std::format("ATRAC3plus, {} frames: {} of {} lines differ; {} PCM outputs (Sony {}), {} samples: SNR {:.1f} dB, "
                     "largest difference {:.6f}; pspocket {:.2f} s, Sony {:.2f} s (interpreted)",
        aus.size(), differ, theirs.lines.size(), ours.pcm.size(), theirs.pcm.size(), samples, snr, max_diff,
        std::chrono::duration<double>(t1 - t0).count(), std::chrono::duration<double>(t2 - t1).count()));
}

// ATRAC3plus frames built here: a valid silent stereo frame (a zero start bit; one stereo channel unit of `units`
// quantisation units, every word length zero, no flags, gain data, tones or noise; the terminator) and an invalid one
// (the start bit set). The decoder's flow: the first frame of the sequence silent, the invalid frame an error and the
// frame after it silent, the sizes and information Sony's.
std::vector<u8> atrac3plus_silent_frame(u32 units)
{
    std::vector<u8> f(0x2B0, 0);
    u32 bit = 0;
    const auto put = [&](u32 v, u32 n) {
        for (u32 i = 0; i < n; i++, bit++)
            if ((v >> (n - 1 - i)) & 1) f[bit / 8] |= static_cast<u8>(0x80 >> (bit % 8));
    };
    put(0, 1);         // start bit
    put(1, 2);         // a stereo channel unit
    put(units - 1, 5); // quantisation units
    put(0, 1);         // not muted
    for (u32 ch = 0; ch < 2; ch++) {
        put(0, 2); // word lengths coded directly
        for (u32 q = 0; q < units; q++) put(0, 3);
    }
    put(0, 2); // no swapped or negated subbands
    put(0, 2); // window shapes
    put(0, 2); // no gain control data
    put(0, 1); // no tones
    put(0, 1); // no noise
    put(3, 2); // terminator
    return f;
}

std::vector<std::vector<u8>> atrac3plus_frames()
{
    std::vector<std::vector<u8>> aus;
    for (const u32 units : {1u, 8u, 28u, 0u, 32u, 16u}) {
        if (units == 0) {
            std::vector<u8> bad(0x2B0, 0);
            bad[0] = 0x80; // the start bit set
            aus.push_back(std::move(bad));
        } else {
            aus.push_back(atrac3plus_silent_frame(units));
        }
    }
    return aus;
}

TEST_CASE("cellAdec: ATRAC3plus on FFmpeg, its flow on frames built here (D-198)", "[hle][adec]")
{
#if !PSPOCKET_HAVE_FFMPEG
    SKIP("no FFmpeg in this build (scripts/build-ffmpeg.sh): pspocket has no ATRAC3plus core");
#endif
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    // 48 kHz, stereo, 688-byte frames, floats, no ATS headers.
    const Observed o = core_run(f.base(), hle_calls(f, t), 3, {48000, 2, 2, 0x2B0, 0, 0x84, 0}, atrac3plus_frames());
    INFO(all_lines(o.lines));
    std::string all = all_lines(o.lines);
    CHECK(o.lines[1] == "attr 3: 0x59f8b 0x492000 0x1020000"); // Sony's
    REQUIRE(o.pcm.size() == 6);
    CHECK(o.pcm[0].empty());        // the sequence's first frame: silence
    CHECK(o.pcm[1].size() == 16384); // 2048 samples, 2 channels, floats
    CHECK(o.pcm[4].empty());        // the frame after the error
    CHECK(o.pcm[5].size() == 16384);
    CHECK(all.find("callback 6: type 2 data 0x806122b2") != std::string::npos); // the invalid frame: an error first
}

TEST_CASE("cellAdec: ATRAC3plus on FFmpeg against Sony's core, on frames built here", "[hle][adec][firmware]")
{
#if !PSPOCKET_HAVE_FFMPEG
    SKIP("no FFmpeg in this build (scripts/build-ffmpeg.sh): pspocket has no ATRAC3plus core to compare");
#endif
    const auto flash = test_dev_flash();
    if (!flash) {
        SUCCEED("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
        return;
    }
    InterpretMisses interpret;
    Observed ours, theirs;
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        ours = core_run(f.base(), hle_calls(f, t), 3, {48000, 2, 2, 0x2B0, 0, 0x84, 0}, atrac3plus_frames());
    }
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        FirmwareLibrary sony(f, t, *flash, "cellAdec");
        theirs = core_run(f.base(),
            [&](const char*, u32 nid, std::initializer_list<u64> args) { return sony.call(t, nid, args); }, 3,
            {48000, 2, 2, 0x2B0, 0, 0x84, 0}, atrac3plus_frames());
    }
    INFO(all_lines(theirs.lines));
    CHECK(compare(ours, theirs) == 0);
}

// D-591: pspocket's LPCM core through its own exported table, g_cell_adec_core_ops_lpcm, the way a firmware module
// drives a core: pspocket's front end on the table (its twelve entries called as guest code, the notifications back as
// guest calls) gives every answer, item, information block, callback and PCM of the direct path, which calls neither.
TEST_CASE("cellAdec: LPCM through pspocket's own operation table, as a firmware module drives it (D-591)",
    "[hle][adec]")
{
    InterpretMisses interpret;
    constexpr u32 kLpcmOps = 0x369D9D3A;
    REQUIRE(kLpcmOps == recomp::nid_of("g_cell_adec_core_ops_lpcm"));
    Observed direct, tabled;
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        direct = adec_run(f.base(), hle_calls(f, t));
        CHECK(ps_read32(f.base(), kDecoderMem + 0x34) == 0); // the context's core_ops: the core itself
        CHECK(guest_calls(f, "cellAdec", "_pspocket_adec_notify_pcm_out") == 0);
        CHECK(guest_calls(f, "cellAdec", "_CellAdecCoreOpOpen_lpcm") == 0);
    }
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        const u32 table = link_own_table(f, "cellAdec", kLpcmOps);
        // The table: twelve descriptors of the core's operations, in CellAdecCoreOps's order.
        const char* ops[12] = {"GetMemSize", "Open", "Close", "StartSeq", "EndSeq", "DecodeAu", "GetVersion", "Realign",
            "ReleasePcm", "GetPcmHandleNum", "GetBsiInfoSize", "OpenExt"};
        for (u32 op = 0; op < 12; op++) {
            INFO(ops[op]);
            const std::string name = std::format("_CellAdecCoreOp{}_lpcm", ops[op]);
            CHECK(ps_read32(f.base(), table + 4 * op) == f.hle->descriptor_of("cellAdec", recomp::nid_of(name)));
        }
        tabled = adec_run(f.base(), hle_calls(f, t));
        u8* b = f.base();
        CHECK(ps_read32(b, kDecoderMem + 0x34) == table);                   // the last decoder was opened on it
        CHECK(ps_read32(b, ps_read32(b, kDecoderMem + 0x30) + 0x114) == 1); // and notified as guest calls
        for (const char* op : {"GetMemSize", "Open", "Close", "StartSeq", "EndSeq", "DecodeAu", "GetVersion", "Realign",
                 "ReleasePcm", "GetPcmHandleNum", "GetBsiInfoSize"}) {
            INFO(op);
            CHECK(guest_calls(f, "cellAdec", std::format("_CellAdecCoreOp{}_lpcm", op)) > 0);
        }
        CHECK(guest_calls(f, "cellAdec", "_pspocket_adec_notify_au_done") > 20);
        CHECK(guest_calls(f, "cellAdec", "_pspocket_adec_notify_pcm_out") > 20);
        CHECK(guest_calls(f, "cellAdec", "_pspocket_adec_notify_seq_done") == 3);
    }
    INFO(all_lines(tabled.lines));
    CHECK(direct.lines.size() > 200);
    CHECK(direct.pcm.size() > 20);
    CHECK(compare(direct, tabled) == 0);
}

#if PSPOCKET_HAVE_FFMPEG

// D-591: the four ATRAC3plus tables likewise, each for its type; a decoder opened with cellAdecOpenEx reaches the
// table's openExt.
TEST_CASE("cellAdec: ATRAC3plus through pspocket's own operation tables, as a firmware module drives them (D-591)",
    "[hle][adec]")
{
    InterpretMisses interpret;
    const struct {
        u32 type, nid;
        const char* tag;
    } tables[] = {{3, 0x4944AF9A, "atracx"}, {13, 0x076B33AB, "atracx2ch"}, {14, 0x1D210EAA, "atracx6ch"},
        {15, 0xE9A86E54, "atracx8ch"}};
    const std::initializer_list<u32> param = {48000, 2, 2, 0x2B0, 0, 0x84, 0};
    for (const auto& tb : tables) {
        DYNAMIC_SECTION(tb.tag)
        {
            Observed direct, tabled;
            {
                BigFixture f;
                GuestThread t(f, 0, "ppu");
                direct = core_run(f.base(), hle_calls(f, t), tb.type, param, atrac3plus_frames());
            }
            {
                BigFixture f;
                GuestThread t(f, 0, "ppu");
                const u32 table = link_own_table(f, "cellAtracXdec", tb.nid);
                tabled = core_run(f.base(), hle_calls(f, t), tb.type, param, atrac3plus_frames());
                u8* b = f.base();
                CHECK(ps_read32(b, kDecoderMem + 0x34) == table);
                CHECK(guest_calls(f, "cellAdec", std::format("_CellAdecCoreOpDecodeAu_{}", tb.tag)) == 6);
                CHECK(guest_calls(f, "cellAdec", "_pspocket_adec_notify_error") == 1); // the frame with its start bit
                CHECK(guest_calls(f, "cellAdec", "_pspocket_adec_notify_pcm_out") == 6);
                // CellAdecResourceEx (totalMemSize, startAddr, ppuThreadPriority, ppuThreadStackSize, spurs, priority,
                // maxContention) through cellAdecOpenEx: the table's openExt opens the core.
                write_callback(b);
                ps_write32(b, kType, tb.type);
                const Call call = hle_calls(f, t);
                REQUIRE(call("cellAdecQueryAttr", 0x7E4A4A49, {kType, kAttr}) == 0);
                const u32 res[8] = {
                    ps_read32(b, kAttr), kDecoderMem, 1000, 0x4000, 0x00ABC000, 0x01010101, 0x01010101, 1};
                for (u32 i = 0; i < 8; i++) ps_write32(b, kRes + 4 * i, res[i]);
                ps_write32(b, kCb, kOpd);
                ps_write32(b, kCb + 4, kLog);
                CHECK(call("cellAdecOpenEx", 0x8B5551A4, {kType, kRes, kCb, kHandle}) == 0);
                CHECK(guest_calls(f, "cellAdec", std::format("_CellAdecCoreOpOpenExt_{}", tb.tag)) == 1);
                CHECK(call("cellAdecClose", 0x847D2380, {ps_read32(b, kHandle)}) == 0);
            }
            INFO(all_lines(tabled.lines));
            CHECK(direct.pcm.size() == 6);
            CHECK(compare(direct, tabled) == 0);
        }
    }
}

#endif
