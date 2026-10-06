// cellDmux and its PAMF core (D-194) over a synthetic MPEG program stream built here (no game data): AVC video on
// channel 0 and ATRAC3plus audio on channel 0 in 2048-byte packs, a start code and a sync word split between two packs,
// a random access point named by a private stream 2, timestamps with bit 31 set (sign-extended on LLE), a program end
// code. First the demultiplexer alone, then the whole API with the game's callbacks as guest code. Then the same calls
// to Sony's libdmux and libdmuxpamf from the user's installed system software, when PSPOCKET_TEST_DEV_FLASH names it
// (firmware_compare.hpp): every answer, callback and access unit must be the same, down to its address.

#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dmux_pamf.hpp"
#include "firmware_compare.hpp"
#include "hle_test_util.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/ppu/asm.hpp"
#include "pspocket/spu/runtime.h"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::ppu::as;

namespace {

// ---- the stream

constexpr u32 kPack = 0x800;
constexpr u64 kNoTs = ~0ull;

struct Pes {
    u8 stream_id = 0;
    std::vector<u8> data; // after the PES header (private streams: from their private header)
    std::optional<u64> pts, dts;
    int rap_channel = -1; // a system header and a private stream 2 naming this video channel come first
};

void put_ts(std::vector<u8>& out, u8 prefix, u64 ts)
{
    out.push_back(static_cast<u8>(prefix << 4 | ((ts >> 29) & 0x0E) | 1));
    const u16 mid = static_cast<u16>(((ts >> 14) & 0xFFFE) | 1), low = static_cast<u16>(((ts << 1) & 0xFFFE) | 1);
    out.insert(out.end(), {static_cast<u8>(mid >> 8), static_cast<u8>(mid), static_cast<u8>(low >> 8), static_cast<u8>(low)});
}

void put_be16(std::vector<u8>& out, usize v) { out.insert(out.end(), {static_cast<u8>(v >> 8), static_cast<u8>(v)}); }

// One pack: header, maybe the system header and private stream 2, the PES packet, padding to 2048 bytes.
std::vector<u8> pack(const Pes& p)
{
    std::vector<u8> out = {0x00, 0x00, 0x01, 0xBA, 0x44, 0x00, 0x04, 0x00, 0x04, 0x01, 0x01, 0x89, 0xC3, 0xF8};
    if (p.rap_channel >= 0) {
        out.insert(out.end(), {0x00, 0x00, 0x01, 0xBB});
        put_be16(out, 12);
        out.insert(out.end(), {0x80, 0x04, 0xE1, 0x04, 0xE1, 0xFF, 0xE0, 0xE0, 0x00, 0xBD, 0xE0, 0x00});
        REQUIRE(out.size() == 32); // the next start code 16-byte aligned, as libdmuxpamf's SPU reads it
        out.insert(out.end(), {0x00, 0x00, 0x01, 0xBF});
        put_be16(out, 0x10);
        out.insert(out.end(), {0x00, static_cast<u8>(p.rap_channel)});
        out.insert(out.end(), 14, 0xFF);
    }
    const u8 flags = static_cast<u8>((p.pts ? 0x80 : 0) | (p.dts ? 0x40 : 0));
    const u8 header_data_length = static_cast<u8>((p.pts ? 5 : 0) + (p.dts ? 5 : 0));
    out.insert(out.end(), {0x00, 0x00, 0x01, p.stream_id});
    put_be16(out, 3 + header_data_length + p.data.size());
    out.insert(out.end(), {0x81, flags, header_data_length});
    if (p.pts) put_ts(out, p.dts ? 3 : 2, *p.pts);
    if (p.dts) put_ts(out, 1, *p.dts);
    out.insert(out.end(), p.data.begin(), p.data.end());
    REQUIRE(out.size() + 6 <= kPack);
    const usize padding = kPack - out.size() - 6;
    out.insert(out.end(), {0x00, 0x00, 0x01, 0xBE});
    put_be16(out, padding);
    out.insert(out.end(), padding, 0xFF);
    return out;
}

// Access unit bodies never hold a start code prefix (no 0 or 1 bytes) or an ATRAC3plus sync word (no 0x0F).
u8 body_byte(u32 seed, u32 i) { return static_cast<u8>(0x20 + (seed * 37 + i * 11) % 0xD0); }

// AVC: four access units, each from an access unit delimiter (00 00 01 09) on.
constexpr u32 kAvcSizes[4] = {900, 2500, 300, 700};
std::vector<u8> avc_es()
{
    std::vector<u8> es;
    for (u32 u = 0; u < 4; u++) {
        es.insert(es.end(), {0x00, 0x00, 0x01, 0x09, 0x10});
        for (u32 i = 5; i < kAvcSizes[u]; i++) es.push_back(body_byte(u, i));
    }
    return es;
}

// ATRAC3plus: five frames, each a sync word (0x0FD0), the size field ((n + 1) * 8 + 8 bytes with the 8-byte header),
// then the frame.
constexpr u32 kAt3Frame = (0x1F + 1) * 8 + 8, kAt3Frames = 5;
std::vector<u8> at3_es()
{
    std::vector<u8> es;
    for (u32 f = 0; f < kAt3Frames; f++) {
        es.insert(es.end(), {0x0F, 0xD0, 0x00, 0x1F});
        for (u32 i = 4; i < kAt3Frame; i++) es.push_back(body_byte(10 + f, i));
    }
    return es;
}

std::vector<u8> slice(const std::vector<u8>& v, usize from, usize to) { return {v.begin() + static_cast<std::ptrdiff_t>(from), v.begin() + static_cast<std::ptrdiff_t>(to)}; }

std::vector<u8> private_stream(u8 id, std::vector<u8> data)
{
    data.insert(data.begin(), {id, 0x00, 0x00, 0x00}); // the private header: no extra header bytes
    return data;
}

constexpr u64 kPtsHigh = 0x80003000; // bit 31 set: sign-extended from s32 on LLE

// The program stream: video cut inside the second unit, then between 00 00 and 01 09 of the third unit's delimiter;
// audio cut between 0x0F and 0xD0 of the fourth frame's sync word; a program end code last.
std::vector<u8> program_stream()
{
    const std::vector<u8> v = avc_es(), a = at3_es();
    const usize v_cut1 = 1500, v_cut2 = 900 + 2500 + 2, a_cut = 3 * kAt3Frame + 1;
    std::vector<u8> s;
    const auto add = [&](const Pes& p) {
        const std::vector<u8> pk = pack(p);
        s.insert(s.end(), pk.begin(), pk.end());
    };
    add({0xE0, slice(v, 0, v_cut1), 0x1000, 0x0F00, 0});
    add({0xBD, private_stream(0x00, slice(a, 0, a_cut)), 0x1000, std::nullopt});
    add({0xE0, slice(v, v_cut1, v_cut2), 0x2000, std::nullopt});
    add({0xBD, private_stream(0x00, slice(a, a_cut, a.size())), 0x5000, std::nullopt});
    add({0xE0, slice(v, v_cut2, v.size()), kPtsHigh, std::nullopt});
    std::vector<u8> end(kPack, 0xFF);
    end[0] = 0x00, end[1] = 0x00, end[2] = 0x01, end[3] = 0xB9;
    s.insert(s.end(), end.begin(), end.end());
    return s;
}

// ---- the demultiplexer alone

struct Found {
    const u8* at;
    u8 stream_id, private_stream_id;
    std::vector<u8> bytes;
    u64 pts, dts;
    bool rap;
};

class Recorder final : public hle::dmux::PamfDemuxer {
public:
    std::vector<Found> found;
    std::vector<std::string> events;
    u32 au_found_refusals = 0; // refuse the next N, as a full event queue does

protected:
    bool on_au_found(u8 stream_id, u8 private_stream_id, u32, std::span<const u8> au, u64 pts, u64 dts, bool rap, u8,
        std::array<u8, 0x10>) override
    {
        if (au_found_refusals) {
            au_found_refusals--;
            return false;
        }
        found.push_back({au.data(), stream_id, private_stream_id, {au.begin(), au.end()}, pts, dts, rap});
        events.push_back(std::format("au {:02x}/{:02x}", stream_id, private_stream_id));
        return true;
    }
    bool on_demux_done() override { events.emplace_back("demux done"); return true; }
    void on_fatal_error() override { events.emplace_back("fatal"); }
    bool on_flush_done(u8 stream_id, u8 private_stream_id, u32) override
    {
        events.push_back(std::format("flush {:02x}/{:02x}", stream_id, private_stream_id));
        return true;
    }
    bool on_prog_end() override { events.emplace_back("prog end"); return true; }
    void on_au_queue_full() override { events.emplace_back("queue full"); }
};

std::vector<u8> au_bytes(const std::vector<u8>& es, usize from, usize size) { return slice(es, from, from + size); }

constexpr u64 sign_extended(u64 ts) { return static_cast<u64>(static_cast<s64>(static_cast<s32>(static_cast<u32>(ts)))); }

} // namespace

TEST_CASE("cellDmuxPamf: stream IDs to elementary stream types and channels (RPCS3 dmuxPamfStreamIdToTypeChannel)",
    "[hle][dmux]")
{
    using namespace hle::dmux;
    CHECK(stream_id_to_type_channel(0xE0, 0) == std::pair{STREAM_TYPE_INDEX_VIDEO, 0u});
    CHECK(stream_id_to_type_channel(0xEF, 0x55) == std::pair{STREAM_TYPE_INDEX_VIDEO, 15u});
    CHECK(stream_id_to_type_channel(0xBD, 0x41) == std::pair{STREAM_TYPE_INDEX_LPCM, 1u});
    CHECK(stream_id_to_type_channel(0xBD, 0x32) == std::pair{STREAM_TYPE_INDEX_AC3, 2u});
    CHECK(stream_id_to_type_channel(0xBD, 0x03) == std::pair{STREAM_TYPE_INDEX_ATRACX, 3u});
    CHECK(stream_id_to_type_channel(0xBD, 0x2F) == std::pair{STREAM_TYPE_INDEX_USER_DATA, 15u});
    CHECK(stream_id_to_type_channel(0xBD, 0x10).first == STREAM_TYPE_INDEX_INVALID);
    CHECK(stream_id_to_type_channel(0xC0, 0).first == STREAM_TYPE_INDEX_INVALID);
}

TEST_CASE("cellDmuxPamf: AVC and ATRAC3plus access units cut at their delimiters across packs, timestamps, random "
          "access, flush (D-194)",
    "[hle][dmux]")
{
    const std::vector<u8> stream = program_stream(), v = avc_es(), a = at3_es();
    std::vector<u8> v_queue(0xb00c0), a_queue(0x6400);
    Recorder d;
    REQUIRE(d.enable_es(0xE0, 0, true, v_queue, 0x12900, false, 7));
    REQUIRE(d.enable_es(0xBD, 0x00, false, a_queue, 0x1008, false, 8));
    CHECK_FALSE(d.enable_es(0xE0, 0, true, v_queue, 0x12900, false, 9)); // enabled already
    CHECK_FALSE(d.enable_es(0xC0, 0, false, a_queue, 0x1008, false, 9)); // MPEG audio: not PAMF
    CHECK(d.enabled_es_count() == 2);

    d.set_stream(stream, true);
    for (int i = 0; i < 100 && d.has_work(); i++) REQUIRE(d.process_next_pack());
    CHECK_FALSE(d.has_work());

    // Video: the units the next delimiter ends; the last waits for a flush.
    std::vector<Found> video, audio;
    for (const Found& f : d.found) (f.stream_id == 0xE0 ? video : audio).push_back(f);
    REQUIRE(video.size() == 3);
    CHECK(video[0].bytes == au_bytes(v, 0, 900));
    CHECK(video[1].bytes == au_bytes(v, 900, 2500));
    CHECK(video[2].bytes == au_bytes(v, 3400, 300)); // its delimiter came split between two packs
    // A unit takes the timestamps of the PES packet it begins in, once; one that begins after another in the same
    // packet has none. The private stream 2 made the first a random access point. A unit that begins in the bytes
    // kept from the last packet (its delimiter was split) and ends in this one takes none either: the next one takes
    // them (RPCS3 process_pes_packet_data: "complete" counts as a beginning only without cached bytes).
    CHECK(video[0].pts == 0x1000);
    CHECK(video[0].dts == 0x0F00);
    CHECK(video[0].rap);
    CHECK(video[1].pts == kNoTs);
    CHECK_FALSE(video[1].rap);
    CHECK(video[2].pts == kNoTs);
    CHECK(video[2].dts == kNoTs);
    // Audio: the frames the next sync word ends, at the size in their header.
    REQUIRE(audio.size() == kAt3Frames - 1);
    for (u32 f = 0; f + 1 < kAt3Frames; f++) CHECK(audio[f].bytes == au_bytes(a, f * kAt3Frame, kAt3Frame));
    CHECK(audio[0].pts == 0x1000);
    CHECK(audio[1].pts == kNoTs);
    CHECK(audio[0].private_stream_id == 0x00);
    // The program end code, then the end of the stream.
    REQUIRE(d.events.size() >= 2);
    CHECK(d.events[d.events.size() - 2] == "prog end");
    CHECK(d.events.back() == "demux done");

    // Flushing gives the unit still open, then its flush done.
    d.found.clear();
    d.events.clear();
    REQUIRE(d.flush_es(0xE0, 0));
    REQUIRE(d.found.size() == 1);
    CHECK(d.found[0].bytes == au_bytes(v, 3700, 700));
    CHECK(d.found[0].pts == sign_extended(kPtsHigh)); // LLE: sign-extended from s32
    CHECK(d.found[0].pts == 0xFFFFFFFF80003000ull);
    CHECK(d.found[0].dts == kNoTs);
    CHECK(d.events == std::vector<std::string>{"au e0/00", "flush e0/00"});
    REQUIRE(d.flush_es(0xBD, 0x00));
    REQUIRE(d.found.size() == 2);
    CHECK(d.found[1].bytes == au_bytes(a, (kAt3Frames - 1) * kAt3Frame, kAt3Frame));
    CHECK(d.flush_es(0xBD, 0x01) == false); // not enabled
    CHECK(d.disable_es(0xE0, 0));
    CHECK_FALSE(d.disable_es(0xE0, 0));
    CHECK(d.enabled_es_count() == 1);
}

TEST_CASE("cellDmuxPamf: a refused access unit is reported again on the next call, nothing lost", "[hle][dmux]")
{
    const std::vector<u8> stream = program_stream(), v = avc_es();
    std::vector<u8> v_queue(0xb00c0);
    Recorder d;
    REQUIRE(d.enable_es(0xE0, 0, true, v_queue, 0x12900, false, 7));
    d.set_stream(stream, true);
    d.au_found_refusals = 2; // the event queue is full twice
    for (int i = 0; i < 100 && d.has_work(); i++) REQUIRE(d.process_next_pack());
    REQUIRE(d.found.size() == 3);
    CHECK(d.found[0].bytes == au_bytes(v, 0, 900));
    CHECK(d.found[1].bytes == au_bytes(v, 900, 2500));
    CHECK(d.found[2].bytes == au_bytes(v, 3400, 300));
}

TEST_CASE("cellDmuxPamf: a full access unit queue stops the stream until the game releases units", "[hle][dmux]")
{
    const std::vector<u8> stream = program_stream(), v = avc_es();
    // Room for the first two units; less than the maximum unit size (0x100) is left after them, so the queue wraps and
    // the third has no room until the first is released.
    std::vector<u8> v_queue(900 + 2500 + 200);
    Recorder d;
    REQUIRE(d.enable_es(0xE0, 0, true, v_queue, 0x100, false, 7));
    d.set_stream(stream, true);
    for (int i = 0; i < 20 && d.has_work(); i++) REQUIRE(d.process_next_pack());
    CHECK(std::find(d.events.begin(), d.events.end(), "queue full") != d.events.end());
    REQUIRE(d.found.size() == 2);
    CHECK(d.found[0].at == v_queue.data());
    CHECK(d.found[1].at == v_queue.data() + 900);
    // The game releases the first unit: the third is written where the queue wrapped, at its start.
    REQUIRE(d.release_au(0xE0, 0, 900));
    for (int i = 0; i < 20 && d.has_work(); i++) REQUIRE(d.process_next_pack());
    REQUIRE(d.found.size() == 3);
    CHECK(d.found[2].bytes == au_bytes(v, 3400, 300));
    CHECK(d.found[2].at == v_queue.data());
    // The fourth (written as it arrives, found on a flush) needs more room than is before the second: it waits too,
    // until that one is released and the queue unwraps.
    CHECK(d.has_work());
    REQUIRE(d.release_au(0xE0, 0, 2500));
    for (int i = 0; i < 20 && d.has_work(); i++) REQUIRE(d.process_next_pack());
    CHECK_FALSE(d.has_work());
    REQUIRE(d.flush_es(0xE0, 0));
    REQUIRE(d.found.size() == 4);
    CHECK(d.found[3].bytes == au_bytes(v, 3700, 700));
    CHECK(d.found[3].at == v_queue.data() + 300);
}

// ---- the API, with the game's callbacks as guest code

namespace {

// Guest functions without lifted code run in the interpreters while this lives (the default policies abort).
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

constexpr u32 kCode = kData, kOpdDmux = kData + 0x400, kOpdEs = kData + 0x410;
constexpr u32 kLogDmux = kData + 0x1000, kLogV = kData + 0x2000, kLogA = kData + 0x3000;
constexpr u32 kArgs = kData + 0x4000, kType = kArgs, kType2 = kArgs + 0x10, kAttr = kArgs + 0x20, kRes = kArgs + 0x40,
              kCb = kArgs + 0x60, kHandle = kArgs + 0x70, kFilterV = kArgs + 0x80, kFilterA = kArgs + 0x90,
              kSpecV = kArgs + 0xA0, kEsAttr = kArgs + 0xB0, kEsResV = kArgs + 0xC0, kEsResA = kArgs + 0xD0,
              kEsCbV = kArgs + 0xE0, kEsCbA = kArgs + 0xF0, kEsHandleV = kArgs + 0x100, kEsHandleA = kArgs + 0x104,
              kAuOut = kArgs + 0x110, kSpecOut = kArgs + 0x114, kEsResA2 = kArgs + 0x120, kEsHandleA2 = kArgs + 0x128;
constexpr u32 kStream = kData + 0x8000, kDmuxMem = kData + 0x10000, kDmuxMemSize = 0x40000, kEsMemV = kData + 0x50000,
              kEsMemVSize = 0xC0000, kEsMemA = kData + 0x110000, kEsMemASize = 0x10000, kEsMemA2 = kData + 0x120000;
constexpr u64 kUserData = 0x12345678;

constexpr u32 CELL_DMUX_ERROR_ARG = 0x80610201, CELL_DMUX_ERROR_SEQ = 0x80610202, CELL_DMUX_ERROR_BUSY = 0x80610203,
              CELL_DMUX_ERROR_EMPTY = 0x80610204;

// The callbacks append (message type, handle, supplemental information) to the log their argument names: a count,
// then 16-byte entries from +16. demux(handle r3, msg r4, arg r5); es(handle r3, es r4, msg r5, arg r6).
void write_callbacks(u8* b)
{
    const std::vector<u32> code = {
        // the demuxer's
        as::lwz(9, 0, 5), as::slwi(10, 9, 4), as::add(10, 10, 5), as::lwz(6, 0, 4), as::stw(6, 16, 10),
        as::stw(3, 20, 10), as::ld(7, 8, 4), as::std_(7, 24, 10), as::lwsync(), as::addi(9, 9, 1), as::stw(9, 0, 5),
        as::li(3, 0), as::blr(),
        // the elementary streams'
        as::lwz(9, 0, 6), as::slwi(10, 9, 4), as::add(10, 10, 6), as::lwz(7, 0, 5), as::stw(7, 16, 10),
        as::stw(4, 20, 10), as::ld(8, 8, 5), as::std_(8, 24, 10), as::lwsync(), as::addi(9, 9, 1), as::stw(9, 0, 6),
        as::li(3, 0), as::blr(),
    };
    for (usize i = 0; i < code.size(); i++) ps_write32(b, kCode + 4 * static_cast<u32>(i), code[i]);
    ps_write32(b, kOpdDmux, kCode);
    ps_write32(b, kOpdDmux + 4, 0);
    ps_write32(b, kOpdEs, kCode + 13 * 4);
    ps_write32(b, kOpdEs + 4, 0);
    for (const u32 log : {kLogDmux, kLogV, kLogA}) std::memset(b + log, 0, 0x1000);
}

u32 log_count(u8* b, u32 log)
{
    const u32 n = ps_read32(b, log);
    std::atomic_thread_fence(std::memory_order_acquire);
    return n;
}

struct LogEntry {
    u32 type, handle;
    u64 supplemental;
};
LogEntry log_entry(u8* b, u32 log, u32 i)
{
    return {ps_read32(b, log + 16 + 16 * i), ps_read32(b, log + 20 + 16 * i), ps_read64(b, log + 24 + 16 * i)};
}

bool wait_until(const std::function<bool()>& done, std::chrono::seconds timeout = std::chrono::seconds(5))
{
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Waits for a callback of `type` beyond the first `seen` in `log`.
bool wait_for_message(u8* b, u32 log, u32 type, u32 seen)
{
    return wait_until([&] {
        const u32 n = log_count(b, log);
        for (u32 i = seen; i < n; i++)
            if (log_entry(b, log, i).type == type) return true;
        return false;
    });
}

std::string hex_bytes(const u8* p, u32 n)
{
    std::string s;
    for (u32 i = 0; i < n; i++) s += std::format("{:02x}", p[i]);
    return s;
}

using Call = std::function<u32(const char* name, u32 nid, std::initializer_list<u64> args)>;

// The whole life of a demuxer, every observation a line; the same lines from pspocket's and Sony's libraries.
std::vector<std::string> dmux_flow(u8* b, const Call& call)
{
    std::vector<std::string> out;
    const auto r = [&](const char* name, u32 nid, std::initializer_list<u64> args) {
        const u32 v = call(name, nid, args);
        out.push_back(std::format("{} -> {:#x}", name, v));
        return v;
    };
    const auto note = [&](std::string s) { out.push_back(std::move(s)); };
    // The callbacks so far, in each one's order.
    const auto note_callbacks = [&] {
        for (const auto& [which, log] : {std::pair{"demuxer", kLogDmux}, std::pair{"video", kLogV}, std::pair{"audio", kLogA}}) {
            const u32 n = log_count(b, log);
            for (u32 i = 0; i < n; i++) {
                const LogEntry e = log_entry(b, log, i);
                note(std::format("{} callback {}: type {} handle {:#x} supplemental {:#x}", which, i, e.type, e.handle,
                    e.supplemental));
            }
        }
    };
    write_callbacks(b);
    const std::vector<u8> stream = program_stream(), v = avc_es(), a = at3_es();
    std::memcpy(b + kStream, stream.data(), stream.size());
    std::memset(b + kArgs, 0, 0x200);

    // Sizes and versions.
    ps_write32(b, kType, 1); // CELL_DMUX_STREAM_TYPE_PAMF
    r("cellDmuxQueryAttr", 0xA2D4189B, {kType, kAttr});
    note(std::format("attr {:#x} {:#x} {:#x}", ps_read32(b, kAttr), ps_read32(b, kAttr + 4), ps_read32(b, kAttr + 8)));
    ps_write32(b, kType2, 1);
    r("cellDmuxQueryAttr2", 0x3F76E3CD, {kType2, kAttr});
    note(std::format("attr2 {:#x}", ps_read32(b, kAttr)));
    r("cellDmuxQueryAttr", 0xA2D4189B, {0, kAttr});
    ps_write32(b, kType, 2);
    r("cellDmuxQueryAttr", 0xA2D4189B, {kType, kAttr});
    ps_write32(b, kType, 1);
    // AVC level 2.1 (CellCodecEsFilterId: stream ID, private stream ID, is AVC), ATRAC3plus with no information.
    const u32 filters[2][4] = {{0xE0, 0, 1, 0}, {0xBD, 0x00, 0, 0}};
    for (u32 i = 0; i < 4; i++) ps_write32(b, kFilterV + 4 * i, filters[0][i]), ps_write32(b, kFilterA + 4 * i, filters[1][i]);
    ps_write32(b, kSpecV, 21);
    r("cellDmuxQueryEsAttr", 0x02170D1A, {kType, kFilterV, kSpecV, kEsAttr});
    note(std::format("video es attr {:#x}", ps_read32(b, kEsAttr)));
    r("cellDmuxQueryEsAttr", 0x02170D1A, {kType, kFilterA, 0, kEsAttr});
    note(std::format("audio es attr {:#x}", ps_read32(b, kEsAttr)));
    r("cellDmuxQueryEsAttr2", 0x52911BCF, {kType2, kFilterA, 0, kEsAttr});
    note(std::format("audio es attr2 {:#x}", ps_read32(b, kEsAttr)));

    // Open: too little memory, then enough (CellDmuxResource: memory, PPU priority and stack, SPU priority, SPUs).
    const u32 res[6] = {kDmuxMem, 0x1000, 1000, 0x4000, 200, 1};
    for (u32 i = 0; i < 6; i++) ps_write32(b, kRes + 4 * i, res[i]);
    ps_write32(b, kCb, kOpdDmux);
    ps_write32(b, kCb + 4, kLogDmux);
    r("cellDmuxOpen", 0x68492DE9, {kType, kRes, kCb, kHandle});
    ps_write32(b, kRes + 4, kDmuxMemSize);
    if (r("cellDmuxOpen", 0x68492DE9, {kType, kRes, kCb, kHandle}) != 0) return out;
    const u32 handle = ps_read32(b, kHandle);
    note(std::format("handle {:#x}", handle));

    // The two streams.
    ps_write32(b, kEsResV, kEsMemV);
    ps_write32(b, kEsResV + 4, 0x1000);
    ps_write32(b, kEsCbV, kOpdEs);
    ps_write32(b, kEsCbV + 4, kLogV);
    r("cellDmuxEnableEs", 0x7B56DC3F, {handle, kFilterV, kEsResV, kEsCbV, kSpecV, kEsHandleV});
    ps_write32(b, kEsResV + 4, kEsMemVSize);
    if (r("cellDmuxEnableEs", 0x7B56DC3F, {handle, kFilterV, kEsResV, kEsCbV, kSpecV, kEsHandleV}) != 0) return out;
    ps_write32(b, kEsResA, kEsMemA);
    ps_write32(b, kEsResA + 4, kEsMemASize);
    ps_write32(b, kEsCbA, kOpdEs);
    ps_write32(b, kEsCbA + 4, kLogA);
    if (r("cellDmuxEnableEs", 0x7B56DC3F, {handle, kFilterA, kEsResA, kEsCbA, 0, kEsHandleA}) != 0) return out;
    const u32 es_v = ps_read32(b, kEsHandleV), es_a = ps_read32(b, kEsHandleA);
    note(std::format("es handles {:#x} {:#x}", es_v, es_a));
    // The same stream again, in memory of its own: libdmux writes the new context's mutex before the core refuses it,
    // so the same memory would lose the first's (RPCS3 cellDmuxEnableEs, as LLE).
    ps_write32(b, kEsResA2, kEsMemA2);
    ps_write32(b, kEsResA2 + 4, kEsMemASize);
    r("cellDmuxEnableEs", 0x7B56DC3F, {handle, kFilterA, kEsResA2, kEsCbA, 0, kEsHandleA2});

    // Nothing to get yet; not stopped, so no flush.
    r("cellDmuxGetAu", 0x42C716B5, {es_v, kAuOut, kSpecOut});
    r("cellDmuxReleaseAu", 0x24EA6474, {es_v});
    r("cellDmuxResetStream", 0x5D345DE9, {handle});

    // The stream, to its end.
    if (r("cellDmuxSetStream", 0x04E7499F, {handle, kStream, stream.size(), 0, kUserData}) != 0) return out;
    r("cellDmuxSetStream", 0x04E7499F, {handle, kStream, stream.size(), 0, kUserData}); // still running
    r("cellDmuxFlushEs", 0xEBB3B2BD, {es_v});
    if (!wait_for_message(b, kLogDmux, 0, 0)) {
        note("no DEMUX_DONE");
        note_callbacks();
        return out;
    }

    // The access units, peeked at then taken, in each stream's order. An access unit's information is a
    // CellDmuxAuInfo (address, size, maximum size, random access, user data, PTS, DTS).
    const auto drain = [&](const char* which, u32 es, u32 n_specific) {
        for (u32 i = 0;; i++) {
            ps_write32(b, kAuOut, 0);
            const u32 peek = call("cellDmuxPeekAu", 0x2750C5E0, {es, kAuOut, kSpecOut});
            const u32 peeked = ps_read32(b, kAuOut);
            const u32 got = call("cellDmuxGetAu", 0x42C716B5, {es, kAuOut, kSpecOut});
            if (got != 0) {
                note(std::format("{} GetAu after {} -> {:#x} (peek {:#x})", which, i, got, peek));
                return;
            }
            const u32 info = ps_read32(b, kAuOut), specific = ps_read32(b, kSpecOut);
            const u32 addr = ps_read32(b, info), size = ps_read32(b, info + 4);
            note(std::format("{} au {}: peek {:#x} same {} info {:#x} addr {:#x} size {:#x} rap {} user {:#x} pts {:#x} "
                             "dts {:#x} specific {:#x} [{}] bytes {}",
                which, i, peek, peeked == info, info, addr, size, ps_read8(b, info + 0xC), ps_read64(b, info + 0x10),
                ps_read64(b, info + 0x18), ps_read64(b, info + 0x20), specific, hex_bytes(b + specific, n_specific),
                hex_bytes(b + addr, std::min<u32>(size, 8))));
            const std::vector<u8>& es_bytes = which[0] == 'v' ? v : a;
            const bool contained = std::search(es_bytes.begin(), es_bytes.end(), b + addr, b + addr + size) != es_bytes.end();
            note(std::format("{} au {} is part of the stream: {}", which, i, contained));
            r("cellDmuxReleaseAu", 0x24EA6474, {es});
        }
    };
    drain("video", es_v, 4);
    drain("audio", es_a, 0);
    r("cellDmuxReleaseAu", 0x24EA6474, {es_a}); // nothing taken

    // Flushing gives the last unit of each, then FLUSH_DONE.
    for (const auto& [which, es, log, n] : {std::tuple{"video", es_v, kLogV, 4u}, std::tuple{"audio", es_a, kLogA, 0u}}) {
        const u32 seen = log_count(b, log);
        if (r("cellDmuxFlushEs", 0xEBB3B2BD, {es}) != 0) continue;
        if (!wait_for_message(b, log, 1, seen)) note(std::format("{}: no FLUSH_DONE", which));
        drain(which, es, n);
    }

    note_callbacks();

    // A second run over the same stream after a discontinuity, then a reset while it runs.
    r("cellDmuxResetEs", 0x21D424F0, {es_v});
    r("cellDmuxResetStream", 0x5D345DE9, {handle}); // stopped: nothing to reset
    const u32 seen = log_count(b, kLogDmux);
    if (r("cellDmuxSetStream", 0x04E7499F, {handle, kStream, stream.size(), 1, kUserData + 1}) == 0) {
        r("cellDmuxResetStreamAndWaitDone", 0xCCFF1284, {handle});
        if (!wait_for_message(b, kLogDmux, 0, seen)) note("no second DEMUX_DONE");
        note(std::format("second run: {} demuxer callbacks", log_count(b, kLogDmux) - seen));
    }

    // Disabling and closing.
    r("cellDmuxDisableEs", 0x05371C8D, {es_a});
    r("cellDmuxDisableEs", 0x05371C8D, {es_a}); // gone
    r("cellDmuxGetAu", 0x42C716B5, {es_a, kAuOut, kSpecOut});
    r("cellDmuxClose", 0x8C692521, {handle});
    r("cellDmuxClose", 0x8C692521, {handle}); // gone
    r("cellDmuxDisableEs", 0x05371C8D, {es_v}); // disabled by the close
    return out;
}

// The NIDs pspocket's flow calls, linked as a game's imports would be.
Call hle_calls(Fixture& f, GuestThread& t)
{
    return [&f, &t](const char* name, u32 nid, std::initializer_list<u64> args) {
        const u32 descriptor = f.hle->link_function("cellDmux", nid, name);
        REQUIRE(descriptor != 0);
        return static_cast<u32>(hle::call_guest(t.ctx, descriptor, args));
    };
}

struct DmuxFixture : Fixture {
    DmuxFixture()
    {
        REQUIRE(hle->link_internal_functions({"cellDmux"}));
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
    }
};

const std::string* find_line(const std::vector<std::string>& lines, std::string_view prefix)
{
    for (const std::string& l : lines)
        if (l.starts_with(prefix)) return &l;
    return nullptr;
}

} // namespace

TEST_CASE("cellDmux: a demuxer's whole life through the API, the game's callbacks as guest code (D-194)", "[hle][dmux]")
{
    InterpretMisses interpret;
    DmuxFixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    const std::vector<std::string> lines = dmux_flow(b, hle_calls(f, t));
    std::string all;
    for (const std::string& l : lines) all += l + "\n";
    INFO(all);
    const auto line = [&](std::string_view prefix) {
        const std::string* l = find_line(lines, prefix);
        return l ? *l : std::string("(missing)");
    };
    // Sizes: libdmux's context and 64 stream handles, then libdmuxpamf's, as Sony's libdmux adds them (RPCS3's
    // query_attr has 0x50 more).
    CHECK(line("attr ") == std::format("attr {:#x} 0x260000 0x280000", ((0xc0 + 0x40 * 4 + 0xf) & ~0xfu) + 0x1c + 0x3d880 + 0xe7b + 0xf));
    CHECK(line("attr ") == "attr 0x3e8e6 0x260000 0x280000");
    CHECK(line("cellDmuxQueryAttr -> ") == "cellDmuxQueryAttr -> 0x0");
    CHECK(lines[4] == std::format("cellDmuxQueryAttr -> {:#x}", CELL_DMUX_ERROR_ARG)); // no type
    CHECK(lines[5] == std::format("cellDmuxQueryAttr -> {:#x}", CELL_DMUX_ERROR_ARG)); // not PAMF
    CHECK(line("handle ") == std::format("handle {:#x}", kDmuxMem));
    CHECK(line("es handles ") == std::format("es handles {:#x} {:#x}", kEsMemV, kEsMemA));
    // Three video units and four audio frames, then one more of each from the flush.
    CHECK(line("video GetAu after 3") .starts_with(std::format("video GetAu after 3 -> {:#x}", CELL_DMUX_ERROR_EMPTY)));
    CHECK(line("audio GetAu after 4").starts_with(std::format("audio GetAu after 4 -> {:#x}", CELL_DMUX_ERROR_EMPTY)));
    CHECK(line("video au 0:").find(std::format("size {:#x} rap 1 user {:#x} pts 0x1000 dts 0xf00", 900, kUserData)) != std::string::npos);
    CHECK(line("video au 0:").find("bytes 0000010910") != std::string::npos);
    CHECK(line("audio au 0:").find(std::format("size {:#x}", kAt3Frame)) != std::string::npos);
    for (const std::string& l : lines)
        if (l.find("is part of the stream") != std::string::npos) CHECK(l.ends_with("true"));
    CHECK(line("video au 0: peek 0x0 same true").size() > 0);
    CHECK(line("second run: ") != "(missing)");
    // Running: a second stream is refused, and so is a flush; nothing taken yet, nothing to release.
    const auto at = std::find(lines.begin(), lines.end(), "cellDmuxSetStream -> 0x0");
    REQUIRE(at + 2 < lines.end());
    CHECK(at[1] == std::format("cellDmuxSetStream -> {:#x}", CELL_DMUX_ERROR_BUSY));
    CHECK(at[2] == std::format("cellDmuxFlushEs -> {:#x}", CELL_DMUX_ERROR_SEQ));
    CHECK(at[-1] == std::format("cellDmuxResetStream -> {:#x}", CELL_DMUX_ERROR_SEQ));
    CHECK(at[-2] == std::format("cellDmuxReleaseAu -> {:#x}", CELL_DMUX_ERROR_SEQ));
    CHECK(lines.back() == std::format("cellDmuxDisableEs -> {:#x}", CELL_DMUX_ERROR_ARG));
}

TEST_CASE("cellDmux: the same answers, callbacks and access units as Sony's libdmux and libdmuxpamf (the user's "
          "firmware)",
    "[hle][dmux][firmware]")
{
    const auto flash = test_dev_flash();
    if (!flash) {
        SUCCEED("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
        return;
    }
    InterpretMisses interpret;
    std::vector<std::string> ours, theirs;
    {
        DmuxFixture f;
        GuestThread t(f, 0, "ppu");
        ours = dmux_flow(f.base(), hle_calls(f, t));
    }
    {
        Fixture f;
        GuestThread t(f, 0, "ppu");
        REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
        FirmwareLibrary sony(f, t, *flash, "cellDmux");
        theirs = dmux_flow(f.base(), [&](const char*, u32 nid, std::initializer_list<u64> args) { return sony.call(t, nid, args); });
    }
    usize differ = 0;
    for (usize i = 0; i < std::max(ours.size(), theirs.size()); i++) {
        const std::string o = i < ours.size() ? ours[i] : "(none)", s = i < theirs.size() ? theirs[i] : "(none)";
        if (o == s) continue;
        if (++differ <= 30) UNSCOPED_INFO(std::format("line {}:\n  pspocket: {}\n  Sony:     {}", i, o, s));
    }
    INFO(std::format("{} of {} lines differ", differ, std::max(ours.size(), theirs.size())));
    CHECK(differ == 0);
}
