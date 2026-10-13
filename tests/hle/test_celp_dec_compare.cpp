// The CELP decoders against Sony's cores (D-932): libcelpdec's core (cellCelpDec, MPEG-4 CELP at 16 kHz) and
// libcelp8dec's (cellCelp8Dec, 8 kHz) under cellAdec. The same calls go to pspocket's cellAdec with pspocket's core and
// to Sony's libadec with Sony's core (the user's installed system software); also to Sony's libadec with pspocket's core
// through the core's exported operation table and PTS function, as a game's own libadec would reach it. Compared: every
// answer, every callback, each PCM item and its information, and every PCM byte. At 16 kHz the access units are
// speech-like audio from pspocket's encoder (part 1, Sony's encoder's own bit for bit), at 8 kHz random frames (every
// field's every value in reach: lags whole and fractional, the four signal modes, gains); sent to several of the core's
// decoders (64, or 16), with resets, muting and missing PTS, in each configuration and word size, and refused ones (a
// wrong size, a decoder past the last, before StartSeq, StartSeq's parameters).
//
// No game data: the audio is built here; skipped without the firmware (firmware_compare.hpp).

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <random>
#include <string>
#include <vector>

#include "../../runtime/hle/src/celp_codec.hpp"
#include "../../runtime/hle/src/celp_spu_fp.hpp"
#include "adec_test_util.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::adec_test;

namespace {

constexpr u32 kCelp = 11, kCelp8 = 25;

// Access units of speech-like audio from pspocket's encoder: `count` of configuration `config`.
std::vector<std::vector<u8>> celp_aus(u32 config, u32 count, u32 seed)
{
    hle::celp::RpeEncoder enc;
    hle::celp::spu::Scope scope;
    enc.reset(config);
    const u32 n = enc.cfg.frame_size;
    std::vector<std::vector<u8>> out;
    double ph = 0;
    for (u32 k = 0; out.size() < count; k++) {
        float x[240];
        for (u32 i = 0; i < n; i++) {
            const double t = static_cast<double>(k * n + i) / 16000;
            ph += (120 + 30 * std::sin(t * 3 + seed)) / 16000;
            x[i] = static_cast<float>(std::lround(9000 * std::sin(2 * M_PI * ph) * (0.6 + 0.4 * std::sin(t * 7)) +
                                                  3000 * std::sin(2 * M_PI * 3 * ph + seed)));
        }
        u8 au[64];
        const u32 size = enc.encode(x, au);
        if (size) out.emplace_back(au, au + size);
    }
    return out;
}

// An 8 kHz configuration's access unit bytes: the frame's bits (the LSPs, signal mode and RMS, each subframe's lag,
// pulse positions and signs, gain) rounded up to bytes.
u32 celp8_au_bytes(u32 config)
{
    const hle::celp::MpeConfig& c = hle::celp::kMpeConfigs[config];
    u32 bit[12];
    u8 pos[12][80];
    if (!hle::celp::mpe_pulses(config, bit, pos)) return 0;
    u32 sub = 8 + c.n_pulses + 6;
    for (u32 i = 0; i < c.n_pulses; i++) sub += bit[i];
    return (30 + c.n_subframes * sub + 7) / 8;
}

// `count` random access units of an 8 kHz configuration.
std::vector<std::vector<u8>> celp8_aus(u32 config, u32 count, u32 seed)
{
    std::mt19937 rng(seed);
    std::vector<std::vector<u8>> out(count, std::vector<u8>(celp8_au_bytes(config)));
    for (auto& au : out)
        for (u8& x : au) x = static_cast<u8>(rng() & 0xFF);
    return out;
}

// One access unit sent: its bytes, the user data (the decoder, reset and mute bits) and a PTS (none: all ones).
struct Unit {
    std::vector<u8> bytes;
    u64 user = 0;
    u32 pts = 0;
    bool has_pts = true;
};

// A decoder's life through cellAdec: QueryAttr, Open, the sequences (each its parameters and units), Close; every
// answer, callback, item, information block and PCM.
struct Run {
    u8* b;
    const Call& call;
    Observed o;
    u32 type = kCelp;
    u32 handle = 0, outs = 0, units = 0;

    u32 r(const char* name, u32 nid, std::initializer_list<u64> args)
    {
        const u32 v = call(name, nid, args);
        o.lines.push_back(std::format("{} -> {:#x}", name, v));
        return v;
    }
    void note(std::string s) { o.lines.push_back(std::move(s)); }

    void open()
    {
        std::memset(b + kArgs, 0, 0x200);
        write_callback(b);
        ps_write32(b, kType, type);
        r("cellAdecQueryAttr", 0x7E4A4A49, {kType, kAttr});
        note(std::format("attr: {:#x} {:#x} {:#x}", ps_read32(b, kAttr), ps_read32(b, kAttr + 4), ps_read32(b, kAttr + 8)));
        const u32 res[5] = {ps_read32(b, kAttr), kDecoderMem, 1000, 200, 0x4000};
        for (u32 i = 0; i < 5; i++) ps_write32(b, kRes + 4 * i, res[i]);
        ps_write32(b, kCb, kOpd);
        ps_write32(b, kCb + 4, kLog);
        REQUIRE(r("cellAdecOpen", 0xD00A6988, {kType, kRes, kCb, kHandle}) == 0);
        handle = ps_read32(b, kHandle);
    }

    u32 start(u32 mode, u32 rate, u32 config, u32 word)
    {
        const u32 p[4] = {mode, rate, config, word};
        for (u32 i = 0; i < 4; i++) ps_write32(b, kParam + 4 * i, p[i]);
        const u32 v = r("cellAdecStartSeq", 0x487B613E, {handle, kParam});
        note(std::format("  start {} {} {} {}", mode, rate, config, word));
        return v;
    }

    // Waits for the callbacks a unit gives (AUDONE, then PCMOUT, or ERROR first), then takes each PCM out.
    void settle()
    {
        if (!wait_until([&] { return count_of(b, 0) >= units; })) note("no AUDONE");
        if (!wait_until([&] { return count_of(b, 1) >= outs + 1; }, std::chrono::seconds(3))) {
            for (int quiet = 0; quiet < 10; quiet++) {
                const u32 now = count_of(b, 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                if (count_of(b, 1) != now) quiet = 0;
            }
        }
        for (; outs < count_of(b, 1); outs++) {
            ps_write32(b, kItemOut, 0);
            if (r("cellAdecGetPcmItem", 0xBD75F78B, {handle, kItemOut}) != 0) break;
            const u32 item = ps_read32(b, kItemOut);
            note(std::format("  item at {:+#x}: {}", static_cast<s64>(item) - handle, hex(b + item, 0x30)));
            const u32 bsi = ps_read32(b, item + 0x10);
            note(std::format("  information at {:+#x}: {}", static_cast<s64>(bsi) - handle, hex(b + bsi, 0x10)));
            std::memset(b + kPcm, 0xCC, 0x1000);
            if (r("cellAdecGetPcm", 0x97FF2AF1, {handle, kPcm}) != 0) break;
            const u32 size = ps_read32(b, item + 0xC);
            note(std::format("  pcm: {} bytes", size));
            o.pcm.emplace_back(b + kPcm, b + kPcm + std::min<u32>(size, 0x1000));
        }
    }

    u32 decode(const Unit& u, u32 k)
    {
        const u32 at = kStream + 0x100 * (k % 64);
        std::memcpy(b + at, u.bytes.data(), u.bytes.size());
        ps_write32(b, kAu + 0, at);
        ps_write32(b, kAu + 4, static_cast<u32>(u.bytes.size()));
        ps_write32(b, kAu + 8, u.has_pts ? 0 : 0xFFFFFFFF);
        ps_write32(b, kAu + 12, u.has_pts ? u.pts : 0xFFFFFFFF);
        ps_write64(b, kAu + 16, u.user);
        const u32 v = r("cellAdecDecodeAu", 0x1529E506, {handle, kAu});
        note(std::format("  unit {}: {} bytes, user data {:#x}, pts {}", k, u.bytes.size(), u.user,
            u.has_pts ? std::format("{:#x}", u.pts) : std::string("none")));
        if (v == 0) {
            units++;
            settle();
        }
        return v;
    }

    void end()
    {
        const u32 seq = count_of(b, 3);
        if (r("cellAdecEndSeq", 0xE2EA549B, {handle}) == 0 && !wait_until([&] { return count_of(b, 3) > seq; }))
            note("no SEQDONE");
    }

    void finish()
    {
        r("cellAdecClose", 0x847D2380, {handle});
        for (u32 n = 0; n < log_count(b); n++) {
            const u32 kind = ps_read32(b, kLog + 16 + 16 * n), data = ps_read32(b, kLog + 20 + 16 * n);
            note(std::format("callback {}: type {} data {}", n, kind,
                kind == 0 ? std::format("{:+#x}", static_cast<s64>(data) - handle) : std::format("{:#x}", data)));
        }
    }
};

Observed celp_run(u8* b, const Call& call)
{
    Observed all;
    u32 k = 0;
    // Each configuration and word size on a decoder of its own (a refused unit keeps its frame: cellAdec answers BUSY
    // after four, Sony's libadec as pspocket's).
    for (u32 config = 0; config < 4; config++)
        for (u32 word = 0; word < 2; word++) {
            Run run{b, call, {}};
            run.note(std::format("decoder: configuration {} word size {}", config, word));
            run.open();
            if (config == 0 && word == 0) {
                // Before StartSeq: refused (SEQ); StartSeq's checks.
                run.decode({celp_aus(0, 1, 1)[0], 0, 0x1000}, k++);
                run.start(0, 2, 0, 0);
                run.start(1, 1, 0, 0);
                run.start(1, 2, 4, 0);
                run.start(1, 2, 0, 2);
            }
            const auto aus = celp_aus(config, 12, config * 2 + word + 3);
            REQUIRE(run.start(1, 2, config, word) == 0);
            // A stream on decoder 0 with a PTS each, the fourth without (the core's correction); decoder 5 interleaved;
            // a reset of decoder 0 with a PTS, one without; a muted unit on decoder 63.
            const u32 step = config == 1 ? 900 : 1350;
            for (u32 i = 0; i < 6; i++) run.decode({aus[i], 0, 0x2000 + step * i, i != 3}, k++);
            run.decode({aus[6], 5, 0x9000}, k++);
            run.decode({aus[7], 5, 0, false}, k++);
            run.decode({aus[8], 0, 0, false}, k++);
            run.decode({aus[9], 0x80, 0x20000}, k++);
            run.decode({aus[10], 0x80 | 0x0300, 0, false}, k++);
            run.decode({aus[11], 0x3F | 0x1200, 0x30000}, k++);
            // StartSeq again in the middle (word size 1: the decoders made again).
            if (word == 1) {
                REQUIRE(run.start(1, 2, config, word) == 0);
                run.decode({aus[0], 0, 0, false}, k++);
                run.decode({aus[1], 5, 0x40000}, k++);
            }
            run.end();
            if (config == 3 && word == 1) {
                // After EndSeq: refused; EndSeq again.
                run.decode({aus[2], 0, 0x1000}, k++);
                run.end();
            } else {
                // Refused: decoder 64, 127, a unit a byte short or long.
                REQUIRE(run.start(1, 2, config, word) == 0);
                run.decode({aus[0], 0x40, 0x30000}, k++);
                run.decode({aus[0], 0x7F, 0x30000}, k++);
                std::vector<u8> shorter(aus[0].begin(), aus[0].end() - 1), longer = aus[0];
                longer.push_back(0x55);
                run.decode({shorter, 0, 0x30000}, k++);
                run.decode({longer, 0, 0x30000}, k++);
            }
            run.finish();
            all.lines.insert(all.lines.end(), run.o.lines.begin(), run.o.lines.end());
            all.pcm.insert(all.pcm.end(), run.o.pcm.begin(), run.o.pcm.end());
        }
    return all;
}

// The 8 kHz core: each configuration on a decoder of its own (all float), its 16 decoders.
Observed celp8_run(u8* b, const Call& call)
{
    Observed all;
    u32 k = 0;
    const u32 configs[] = {0, 2, 6, 9, 12, 15, 18, 21, 24, 26};
    for (const u32 config : configs) {
        Run run{b, call, {}, kCelp8};
        run.note(std::format("decoder: configuration {}", config));
        run.open();
        if (config == 0) {
            // Before StartSeq: refused (SEQ); StartSeq's checks.
            run.decode({celp8_aus(0, 1, 1)[0], 0, 0x1000}, k++);
            run.start(1, 5, 0, 0);
            run.start(0, 2, 0, 0);
            run.start(0, 5, 1, 0);
            run.start(0, 5, 27, 0);
            run.start(0, 5, 28, 0);
            run.start(0, 5, 0, 1);
        }
        const auto aus = celp8_aus(config, 12, config * 5 + 3);
        REQUIRE(run.start(0, 5, config, 0) == 0);
        // A stream on decoder 0 with a PTS each, the fourth without (the core's correction); decoder 5 interleaved;
        // a reset of decoder 0 with a PTS, one muted without; muted units on decoder 15.
        const u32 step = hle::celp::kMpeConfigs[config].frame_size * 45 / 4;
        for (u32 i = 0; i < 6; i++) run.decode({aus[i], 0, 0x2000 + step * i, i != 3}, k++);
        run.decode({aus[6], 5, 0x9000}, k++);
        run.decode({aus[7], 5, 0, false}, k++);
        run.decode({aus[8], 0, 0, false}, k++);
        run.decode({aus[9], 0x80, 0x20000}, k++);
        run.decode({aus[10], 0x80 | 0x0300, 0, false}, k++);
        run.decode({aus[11], 0x0F | 0x1200, 0x30000}, k++);
        run.decode({aus[0], 0x0F | 0x0100, 0x31000}, k++);
        run.decode({aus[1], 0x0F | 0x0100, 0, false}, k++);
        // StartSeq again in the middle: the decoders made again.
        if (config == 9 || config == 24) {
            REQUIRE(run.start(0, 5, config, 0) == 0);
            run.decode({aus[2], 0, 0, false}, k++);
            run.decode({aus[3], 5, 0x40000}, k++);
        }
        run.end();
        if (config == 26) {
            // After EndSeq: refused; EndSeq again.
            run.decode({aus[2], 0, 0x1000}, k++);
            run.end();
        } else {
            // Refused: decoder 16, 127, a unit a byte short or long.
            REQUIRE(run.start(0, 5, config, 0) == 0);
            run.decode({aus[0], 0x10, 0x30000}, k++);
            run.decode({aus[0], 0x7F, 0x30000}, k++);
            std::vector<u8> shorter(aus[0].begin(), aus[0].end() - 1), longer = aus[0];
            longer.push_back(0x55);
            run.decode({shorter, 0, 0x30000}, k++);
            run.decode({longer, 0, 0x30000}, k++);
        }
        run.finish();
        all.lines.insert(all.lines.end(), run.o.lines.begin(), run.o.lines.end());
        all.pcm.insert(all.pcm.end(), run.o.pcm.begin(), run.o.pcm.end());
    }
    return all;
}

void compare_halves(const Observed& ours, const Observed& theirs, const char* what)
{
    INFO(all_lines(theirs.lines));
    const usize differ = compare(ours, theirs);
    usize bytes = 0;
    for (const auto& p : theirs.pcm) bytes += p.size();
    CHECK(differ == 0);
    WARN(std::format("{}: {} lines and {} PCM outputs ({} bytes), {} differences", what, theirs.lines.size(),
        theirs.pcm.size(), bytes, differ));
}

} // namespace

TEST_CASE("cellCelpDec: its exports are pspocket's own (D-932)", "[hle][celp]")
{
    const hle::Registry& reg = hle::registry();
    CHECK(reg.find_variable("cellCelpDec", 0xFFE42C22) != nullptr);
    CHECK(reg.find("cellCelpDec", 0xE4B8FC4D) != nullptr);
    CHECK(recomp::nid_of("_SceAdecCorrectPtsValue_Celp") == 0xE4B8FC4D);
    CHECK_FALSE(reg.is_firmware_first("cellCelpDec"));
}

// Without the firmware: pspocket's cellAdec on pspocket's core decodes the encoder's speech, every unit's PCM out.
TEST_CASE("cellCelpDec: pspocket's core under pspocket's cellAdec decodes the encoder's speech", "[hle][celp]")
{
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    const Observed o = celp_run(f.base(), hle_calls(f, t));
    INFO(all_lines(o.lines));
    // 8 decoders of 12 units, two more for each word size 1, and the unit after EndSeq (the core's SEQ: no samples).
    CHECK(o.pcm.size() == 8 * 12 + 4 * 2 + 1);
    usize full = 0, silent = 0;
    for (const auto& p : o.pcm) {
        full += !p.empty();
        silent += !p.empty() && std::all_of(p.begin(), p.end(), [](u8 x) { return x == 0; });
    }
    CHECK(full == 8 * 12 + 4 * 2);
    // Silent: only the two muted units of each decoder, each the first frame of a decoder made fresh (its excitation all
    // in the pulses muting zeroes).
    CHECK(silent == 8 * 2);
    CHECK(find_line(o.lines, "no AUDONE") == nullptr);
}

TEST_CASE("cellCelpDec against Sony's libcelpdec under Sony's libadec: answers, callbacks, items, every PCM byte",
    "[hle][lle][compare][celp]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    InterpretMisses interpret;
    Observed ours, theirs, mixed;
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        ours = celp_run(f.base(), hle_calls(f, t));
    }
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        FirmwareLibrary sony(f, t, *flash, "cellAdec", true, {"cellCelpDec"});
        theirs = celp_run(f.base(),
            [&](const char*, u32 nid, std::initializer_list<u64> args) { return sony.call(t, nid, args); });
    }
    {
        // Sony's libadec on pspocket's core: its imports of cellCelpDec go to pspocket's exports.
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        FirmwareLibrary sony(f, t, *flash, "cellAdec", true);
        mixed = celp_run(f.base(),
            [&](const char*, u32 nid, std::initializer_list<u64> args) { return sony.call(t, nid, args); });
    }
    compare_halves(ours, theirs, "pspocket's cellAdec and core against Sony's libadec and libcelpdec");
    compare_halves(mixed, theirs, "Sony's libadec on pspocket's core against Sony's libadec and libcelpdec");
}

TEST_CASE("cellCelp8Dec: its exports are pspocket's own (D-932)", "[hle][celp]")
{
    const hle::Registry& reg = hle::registry();
    CHECK(reg.find_variable("cellCelp8Dec", 0xF0190C6C) != nullptr);
    CHECK(reg.find("cellCelp8Dec", 0x881583C3) != nullptr);
    CHECK(recomp::nid_of("_SceAdecCorrectPtsValue_Celp8") == 0x881583C3);
    CHECK_FALSE(reg.is_firmware_first("cellCelp8Dec"));
    // The access units' sizes, MPE_Configuration 0 to 27, from the frames' bit allocations.
    const u32 bytes[28] = {20, 22, 24, 19, 20, 21, 15, 15, 16, 17, 18, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 27, 14,
        15, 15, 15, 16, 24};
    for (u32 c = 0; c < 28; c++) {
        INFO("configuration " << c);
        CHECK(celp8_au_bytes(c) == bytes[c]);
    }
}

// Without the firmware: pspocket's cellAdec on pspocket's 8 kHz core, every unit's PCM out.
TEST_CASE("cellCelp8Dec: pspocket's core under pspocket's cellAdec decodes every unit", "[hle][celp]")
{
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    const Observed o = celp8_run(f.base(), hle_calls(f, t));
    INFO(all_lines(o.lines));
    // 10 decoders of 14 units, two more for configurations 9 and 24, the unit after EndSeq (the core's SEQ: no samples).
    CHECK(o.pcm.size() == 10 * 14 + 2 * 2 + 1);
    usize full = 0;
    for (const auto& p : o.pcm) full += !p.empty();
    CHECK(full == 10 * 14 + 2 * 2);
    CHECK(find_line(o.lines, "no AUDONE") == nullptr);
}

TEST_CASE("cellCelp8Dec against Sony's libcelp8dec under Sony's libadec: answers, callbacks, items, every PCM byte",
    "[hle][lle][compare][celp]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    InterpretMisses interpret;
    Observed ours, theirs, mixed;
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        ours = celp8_run(f.base(), hle_calls(f, t));
    }
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        FirmwareLibrary sony(f, t, *flash, "cellAdec", true, {"cellCelp8Dec"});
        theirs = celp8_run(f.base(),
            [&](const char*, u32 nid, std::initializer_list<u64> args) { return sony.call(t, nid, args); });
    }
    {
        // Sony's libadec on pspocket's core: its imports of cellCelp8Dec go to pspocket's exports.
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        FirmwareLibrary sony(f, t, *flash, "cellAdec", true);
        mixed = celp8_run(f.base(),
            [&](const char*, u32 nid, std::initializer_list<u64> args) { return sony.call(t, nid, args); });
    }
    compare_halves(ours, theirs, "pspocket's cellAdec and core against Sony's libadec and libcelp8dec");
    compare_halves(mixed, theirs, "Sony's libadec on pspocket's core against Sony's libadec and libcelp8dec");
}
