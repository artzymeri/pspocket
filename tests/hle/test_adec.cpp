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

#include "firmware_compare.hpp"
#include "hle_test_util.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/ppu/asm.hpp"
#include "pspocket/spu/runtime.h"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::ppu::as;

namespace {

struct InterpretMisses {
    const ps_ppu_miss_policy ppu = ps_ppu_get_miss_policy();
    const ps_spu_miss_policy spu = ps_spu_get_miss_policy();
    InterpretMisses()
    {
        ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
        ps_spu_set_miss_policy(PS_SPU_MISS_INTERPRET);
    }
    ~InterpretMisses()
    {
        ps_ppu_set_miss_policy(ppu);
        ps_spu_set_miss_policy(spu);
    }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

constexpr u32 kCode = kData, kOpd = kData + 0x400, kLog = kData + 0x1000;
constexpr u32 kArgs = kData + 0x4000, kType = kArgs, kAttr = kArgs + 0x10, kRes = kArgs + 0x20, kCb = kArgs + 0x40,
              kHandle = kArgs + 0x50, kParam = kArgs + 0x60, kAu = kArgs + 0x80, kItemOut = kArgs + 0xB0;
constexpr u32 kStream = kData + 0x10000; // access units, 0x2000 bytes each
constexpr u32 kPcm = kData + 0x40000;    // GetPcm's output
constexpr u32 kDecoderMem = 0x01000000, kDecoderMemSize = 0x00800000;

// The callback (handle r3, msgType r4, msgData r5, cbArg r6) appends (type, data, handle) to the log at cbArg.
void write_callback(u8* b)
{
    const std::vector<u32> code = {
        as::lwz(9, 0, 6), as::slwi(10, 9, 4), as::add(10, 10, 6), as::stw(4, 16, 10), as::stw(5, 20, 10),
        as::stw(3, 24, 10), as::lwsync(), as::addi(9, 9, 1), as::stw(9, 0, 6), as::li(3, 0), as::blr(),
    };
    for (usize i = 0; i < code.size(); i++) ps_write32(b, kCode + 4 * static_cast<u32>(i), code[i]);
    ps_write32(b, kOpd, kCode);
    ps_write32(b, kOpd + 4, 0);
    std::memset(b + kLog, 0, 0x1000);
}

u32 log_count(u8* b)
{
    const u32 n = ps_read32(b, kLog);
    std::atomic_thread_fence(std::memory_order_acquire);
    return n;
}

u32 count_of(u8* b, u32 type)
{
    u32 n = 0;
    for (u32 i = 0; i < log_count(b); i++) n += ps_read32(b, kLog + 16 + 16 * i) == type;
    return n;
}

bool wait_until(const std::function<bool()>& done, std::chrono::seconds timeout = std::chrono::seconds(30))
{
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

std::string hex(const u8* p, u32 n)
{
    std::string s;
    for (u32 i = 0; i < n; i++) s += std::format("{:02x}", p[i]);
    return s;
}

struct BigFixture : Fixture {
    BigFixture()
    {
        REQUIRE(hle->link_internal_functions({"cellAdec"}));
        REQUIRE(hle->main_memory().reserve(kDecoderMem, kDecoderMemSize));
        REQUIRE(mem.commit(kDecoderMem, kDecoderMemSize));
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
    }
};

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

using Call = std::function<u32(const char* name, u32 nid, std::initializer_list<u64> args)>;

// pspocket's cellAdec, linked as a game's imports would be.
Call hle_calls(Fixture& f, GuestThread& t)
{
    return [&f, &t](const char* name, u32 nid, std::initializer_list<u64> args) {
        const u32 descriptor = f.hle->link_function("cellAdec", nid, name);
        REQUIRE(descriptor != 0);
        return static_cast<u32>(hle::call_guest(t.ctx, descriptor, args));
    };
}

// What a decoder's life shows: every answer, item, information block and callback as a line (compared exactly), and
// each PCM taken (compared byte by byte).
struct Observed {
    std::vector<std::string> lines;
    std::vector<std::vector<u8>> pcm;
};

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

// A decoder of `type` on a firmware core: its answers, items, information blocks (0x40 bytes), PCM and callbacks.
Observed core_run(u8* b, const Call& call, u32 type, std::initializer_list<u32> param, const std::vector<std::vector<u8>>& aus)
{
    Observed o;
    const auto r = [&](const char* name, u32 nid, std::initializer_list<u64> args) {
        const u32 v = call(name, nid, args);
        o.lines.push_back(std::format("{} -> {:#x}", name, v));
        return v;
    };
    const auto note = [&](std::string s) { o.lines.push_back(std::move(s)); };
    std::memset(b + kArgs, 0, 0x200);
    write_callback(b);
    ps_write32(b, kType, type);
    r("cellAdecQueryAttr", 0x7E4A4A49, {kType, kAttr});
    note(std::format("attr {}: {:#x} {:#x} {:#x}", type, ps_read32(b, kAttr), ps_read32(b, kAttr + 4), ps_read32(b, kAttr + 8)));
    const u32 res[5] = {ps_read32(b, kAttr), kDecoderMem, 1000, 200, 0x4000};
    for (u32 i = 0; i < 5; i++) ps_write32(b, kRes + 4 * i, res[i]);
    ps_write32(b, kCb, kOpd);
    ps_write32(b, kCb + 4, kLog);
    if (r("cellAdecOpen", 0xD00A6988, {kType, kRes, kCb, kHandle}) != 0) return o;
    const u32 handle = ps_read32(b, kHandle);
    u32 i = 0;
    for (const u32 v : param) ps_write32(b, kParam + 4 * i++, v);
    if (r("cellAdecStartSeq", 0x487B613E, {handle, kParam}) != 0) {
        r("cellAdecClose", 0x847D2380, {handle});
        return o;
    }
    u32 outs = 0;
    for (u32 k = 0; k < aus.size(); k++) {
        const u32 at = kStream + 0x2000 * k;
        std::memcpy(b + at, aus[k].data(), aus[k].size());
        ps_write32(b, kAu + 0, at);
        ps_write32(b, kAu + 4, static_cast<u32>(aus[k].size()));
        ps_write32(b, kAu + 8, k == 2 ? 0xFFFFFFFF : 0); // the third without a PTS: the core's correction
        ps_write32(b, kAu + 12, k == 2 ? 0xFFFFFFFF : 0x1000 + 2880 * k);
        ps_write64(b, kAu + 16, 0x77000000 + k);
        if (r("cellAdecDecodeAu", 0x1529E506, {handle, kAu}) != 0) continue;
        if (!wait_until([&] { return count_of(b, 0) >= k + 1; })) note("no AUDONE");
        // The core may hold an access unit back: take what is out once it settles.
        for (int quiet = 0; quiet < 25; quiet++) {
            const u32 now = count_of(b, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (count_of(b, 1) != now) quiet = 0;
        }
        for (; outs < count_of(b, 1); outs++) {
            ps_write32(b, kItemOut, 0);
            if (r("cellAdecGetPcmItem", 0xBD75F78B, {handle, kItemOut}) != 0) break;
            const u32 item = ps_read32(b, kItemOut);
            note(std::format("item at {:+#x}: {}", static_cast<s64>(item) - handle, hex(b + item, 0x30)));
            const u32 bsi = ps_read32(b, item + 0x10);
            note(std::format("  information at {:+#x}: {}", static_cast<s64>(bsi) - handle, hex(b + bsi, 0x40)));
            std::memset(b + kPcm, 0xCC, 0x10000);
            if (r("cellAdecGetPcm", 0x97FF2AF1, {handle, kPcm}) != 0) break;
            const u32 size = ps_read32(b, item + 0xC);
            note(std::format("  pcm: {} bytes", size));
            o.pcm.emplace_back(b + kPcm, b + kPcm + std::min<u32>(size, 0x10000));
        }
    }
    r("cellAdecEndSeq", 0xE2EA549B, {handle});
    if (!wait_until([&] { return count_of(b, 3) >= 1; })) note("no SEQDONE");
    for (u32 n = 0; n < log_count(b); n++) {
        const u32 type_n = ps_read32(b, kLog + 16 + 16 * n), data = ps_read32(b, kLog + 20 + 16 * n);
        note(std::format("callback {}: type {} data {}", n, type_n,
            type_n == 0 ? std::format("{:+#x}", static_cast<s64>(data) - handle) : std::format("{:#x}", data)));
    }
    r("cellAdecClose", 0x847D2380, {handle});
    return o;
}

// The firmware's codec cores linked for pspocket's cellAdec as the loader links them for a game (D-197), started.
struct FirmwareCores {
    loader::LleModules lle;
    FirmwareCores(Fixture& f, GuestThread& t, const std::string& dev_flash) : lle(dev_flash)
    {
        REQUIRE(lle.index());
        lle.link_host_imports({{"cellAdec", {}, {}}}, *f.hle);
        for (const u32 start : lle.take_starts()) hle::call_guest(t.ctx, start, {0, 0});
    }
};

usize compare(const Observed& ours, const Observed& theirs)
{
    usize differ = 0;
    for (usize i = 0; i < std::max(ours.lines.size(), theirs.lines.size()); i++) {
        const std::string x = i < ours.lines.size() ? ours.lines[i] : "(none)";
        const std::string y = i < theirs.lines.size() ? theirs.lines[i] : "(none)";
        if (x == y) continue;
        if (++differ <= 40) UNSCOPED_INFO(std::format("line {}:\n  pspocket: {}\n  Sony:     {}", i, x, y));
    }
    if (ours.pcm.size() != theirs.pcm.size()) {
        UNSCOPED_INFO(std::format("{} PCM outputs, Sony {}", ours.pcm.size(), theirs.pcm.size()));
        return differ + 1;
    }
    for (usize i = 0; i < ours.pcm.size(); i++) {
        if (ours.pcm[i] == theirs.pcm[i]) continue;
        usize first = 0;
        while (first < std::min(ours.pcm[i].size(), theirs.pcm[i].size()) && ours.pcm[i][first] == theirs.pcm[i][first])
            first++;
        if (++differ <= 60)
            UNSCOPED_INFO(std::format("pcm {} differs ({} and {} bytes), from byte {}", i, ours.pcm[i].size(),
                theirs.pcm[i].size(), first));
    }
    return differ;
}

std::string all_lines(const std::vector<std::string>& lines)
{
    std::string s;
    for (const std::string& l : lines) s += l + "\n";
    return s;
}

const std::string* find_line(const std::vector<std::string>& lines, std::string_view prefix)
{
    for (const std::string& l : lines)
        if (l.starts_with(prefix)) return &l;
    return nullptr;
}

float be_float(const std::vector<u8>& v, usize i)
{
    const u32 bits = static_cast<u32>(v[4 * i]) << 24 | static_cast<u32>(v[4 * i + 1]) << 16 |
        static_cast<u32>(v[4 * i + 2]) << 8 | v[4 * i + 3];
    float f;
    std::memcpy(&f, &bits, 4);
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
