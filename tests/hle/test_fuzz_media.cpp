// Mutation fuzzing of the movie and video parsers (D-603, tests/common/mutation_fuzz.hpp): PAMF headers through
// cellPamf (every reader call over each header, the header flush against unmapped memory), the PAMF demultiplexer's
// pack, PES and access unit parsing over damaged program streams, and cellVdec's own stream parsing in front of
// FFmpeg (H.264's SPS, PPS and SEI, MVC's views left out, VC-1's sequence headers, entry points and picture headers)
// over damaged access units in a decoder's sequences, and the audio cores' own frame parsing in front of FFmpeg. Synthetic streams only (pamf_writer.hpp, h264_writer.hpp,
// vc1_writer.hpp). Longer: PSPOCKET_FUZZ_ITERS=<n> build/host/tests/hle/test_hle "[fuzz]".

#include <catch_amalgamated.hpp>

#include <chrono>
#include <cstring>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "atrac_writer.hpp"
#include "dmux_pamf.hpp"
#include "hle_fuzz_util.hpp"
#include "pamf_writer.hpp"

#if PSPOCKET_HAVE_FFMPEG
#include <cmath>

#include "adec_ffmpeg.hpp" // runtime/hle/src (internal): the cores' codecs
#include "h264_writer.hpp"
#include "vc1_writer.hpp"
#include "vdec_test_util.hpp"
#endif

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::fuzzing;

namespace {

// ---- cellPamf

constexpr u32 kReader = kData + 0x20000, kOut = kData + 0x21000, kIt = kData + 0x22000, kEp = kData + 0x23000,
              kTs = kData + 0x24000;

// The header's fields: sizes, the sequence's times and counts, the grouping period's and group's counts, each
// stream header's coding type, IDs, entry point table and codec information, the entry points.
std::vector<fuzz::Field> pamf_fields()
{
    std::vector<fuzz::Field> f = {{0x08, 4}, {0x0C, 4}, {0x10, 4}, {0x14, 4}, {0x50, 4}, {0x56, 2}, {0x58, 4},
        {0x5C, 2}, {0x5E, 4}, {0x62, 4}, {0x6A, 4}, {0x6F, 1}, {0x70, 4}, {0x76, 4}, {0x7C, 4}, {0x81, 1}, {0x82, 4},
        {0x87, 1}};
    for (u32 s = 0x88; s < 0x88 + 2 * 48; s += 48) {
        fuzz::append(f, {{s, 1}, {s + 4, 1}, {s + 5, 1}, {s + 8, 4}, {s + 12, 4}});
        fuzz::append(f, fuzz::fields(s + 16, 32, 1));
    }
    for (u32 e = 0xE8; e < 0xE8 + 4 * 12; e += 12) fuzz::append(f, {{e, 2}, {e + 4, 4}, {e + 8, 4}});
    return f;
}

// Every reader call over the header at `hdr`: the answers that are not errors count it as accepted.
bool pamf_calls(GuestThread& a, u8* b, u32 hdr, u64 file_size, fuzz::Rng& rng)
{
    (void)a.call("cellPamfVerify", hdr, file_size);
    (void)a.call("cellPamfGetHeaderSize", hdr, file_size, kOut);
    (void)a.call("cellPamfGetHeaderSize2", hdr, file_size, rng.below(4), kOut);
    (void)a.call("cellPamfGetStreamOffsetAndSize", hdr, file_size, kOut, kOut + 8);
    std::memset(b + kReader, 0xEE, 0x80);
    const u32 attribute = static_cast<u32>(rng.below(4));
    if (a.call("cellPamfReaderInitialize", kReader, hdr, file_size, attribute) != 0) return false;
    (void)a.call("cellPamfReaderGetPresentationStartTime", kReader, kTs);
    (void)a.call("cellPamfReaderGetPresentationEndTime", kReader, kTs);
    (void)a.call("cellPamfReaderGetMuxRateBound", kReader);
    const u32 streams = static_cast<u32>(a.call("cellPamfReaderGetNumberOfStreams", kReader));
    for (const u32 type : {0u, 1u, 2u, 3u, 4u, 5u, 20u, 21u, 22u}) {
        (void)a.call("cellPamfReaderGetNumberOfSpecificStreams", kReader, type);
        (void)a.call("cellPamfReaderSetStreamWithTypeAndChannel", kReader, type, rng.below(2));
        (void)a.call("cellPamfReaderSetStreamWithTypeAndIndex", kReader, type, rng.below(2));
    }
    for (u32 i = 0; i < std::min<u32>(streams, 4) + 1; i++) {
        if (a.call("cellPamfReaderSetStreamWithIndex", kReader, i) != 0) continue;
        (void)a.call("cellPamfReaderGetStreamIndex", kReader);
        (void)a.call("cellPamfReaderGetStreamTypeAndChannel", kReader, kOut, kOut + 1);
        (void)a.call("cellPamfReaderGetEsFilterId", kReader, kOut);
        for (const u32 size : {0x20u, 0x1Fu, 0x100u}) (void)a.call("cellPamfReaderGetStreamInfo", kReader, kOut, size);
        const u32 eps = static_cast<u32>(a.call("cellPamfReaderGetNumberOfEp", kReader));
        for (u32 e = 0; e < std::min<u32>(eps, 4) + 1; e++) {
            (void)a.call("cellPamfReaderGetEpWithIndex", kReader, e, kEp, 0x20);
            if (a.call("cellPamfReaderGetEpIteratorWithIndex", kReader, e, kIt) == 0) {
                (void)a.call("cellPamfEpIteratorGetEp", kIt, kEp);
                (void)a.call("cellPamfEpIteratorMove", kIt, static_cast<u64>(static_cast<s64>(rng.below(9)) - 4), kEp);
            }
        }
        ps_write32(b, kTs, static_cast<u32>(rng.below(2)));
        ps_write32(b, kTs + 4, static_cast<u32>(rng.next()));
        (void)a.call("cellPamfReaderGetEpWithTimeStamp", kReader, kTs, kEp, 0x20);
        if (a.call("cellPamfReaderGetEpIteratorWithTimeStamp", kReader, kTs, kIt) == 0)
            (void)a.call("cellPamfEpIteratorMove", kIt, 1, kEp);
    }
    return true;
}

// ---- the demultiplexer

class Collector final : public hle::dmux::PamfDemuxer {
public:
    u64 units = 0, bytes = 0, refusals = 0;
    fuzz::Rng* rng = nullptr;

protected:
    bool on_au_found(u8, u8, u32, std::span<const u8> au, u64, u64, bool, u8, std::array<u8, 0x10>) override
    {
        if (rng && rng->one_in(16)) { // now and then a full event queue: the demuxer tries again
            refusals++;
            return false;
        }
        units++;
        for (const u8 x : au) bytes += x; // every byte of the unit read: it must lie in its queue
        return true;
    }
    bool on_demux_done() override { return true; }
    void on_fatal_error() override {}
    bool on_flush_done(u8, u8, u32) override { return true; }
    bool on_prog_end() override { return true; }
    void on_au_queue_full() override {}
};

// The fields of a program stream: each pack's PES packet lengths and header lengths, every 2048 bytes.
std::vector<fuzz::Field> program_fields(const std::vector<u8>& s)
{
    std::vector<fuzz::Field> f;
    for (usize pack = 0; pack + pamf::kPack <= s.size(); pack += pamf::kPack) {
        for (usize at = pack + 14; at + 9 <= pack + pamf::kPack; at++) {
            if (s[at] != 0 || s[at + 1] != 0 || s[at + 2] != 1) continue;
            fuzz::append(f, {{at + 3, 1}, {at + 4, 2}, {at + 7, 1}, {at + 8, 1}, {at + 9, 1}, {at + 10, 4}});
            at += 3;
        }
    }
    return f;
}

} // namespace

TEST_CASE("fuzz: PAMF headers through cellPamf, the header flush against unmapped memory", "[hle][fuzz]")
{
    Fixture f;
    add_guarded_areas(f);
    GuestThread a(f, 0, "ppu");
    u8* b = f.base();
    const std::vector<u8> header = pamf::pamf_header();
    fuzz::Tally tally("cellPamf");
    fuzz::run(tally, {{"pamf header", header, pamf_fields()}}, 2500, [&](const std::vector<u8>& input, fuzz::Rng& rng) {
        const u32 hdr = place_input(b, input);
        const u64 file_size = rng.one_in(3) ? 0 : rng.one_in(2) ? input.size() + 0x8000 : rng.next() & 0xFFFFFF;
        return pamf_calls(a, b, hdr, file_size, rng);
    });
}

TEST_CASE("fuzz: the PAMF demultiplexer over damaged program streams", "[hle][fuzz]")
{
    const std::vector<u8> stream = pamf::program_stream();
    // A raw elementary stream as well: the video alone, no packs.
    const std::vector<u8> es = pamf::avc_es();
    fuzz::Tally tally("pamf demuxer");
    const auto demux = [](const std::vector<u8>& input, fuzz::Rng& rng, bool raw) {
        std::vector<u8> v_queue(0xb00c0), a_queue(0x6400), l_queue(0x6400), u_queue(0x2000);
        Collector d;
        d.rng = &rng;
        bool ok = d.enable_es(0xE0, 0, true, v_queue, 0x12900, raw, 7);
        if (!raw) {
            ok &= d.enable_es(0xBD, 0x00, false, a_queue, 0x1008, false, 8);
            ok &= d.enable_es(0xBD, 0x40, false, l_queue, 0x1008, false, 9);
            ok &= d.enable_es(0xBD, 0x20, false, u_queue, 0x800, false, 10);
        }
        REQUIRE(ok);
        d.set_stream(input, rng.one_in(2));
        // Bounded: a pack at a time, the units taken back as a player releases them.
        bool good = true;
        for (usize i = 0; i < input.size() / 0x200 + 64 && d.has_work(); i++) {
            good &= d.process_next_pack();
            if (rng.one_in(4)) (void)d.release_au(0xE0, 0, static_cast<u32>(rng.below(0x2000)));
            if (rng.one_in(32)) (void)d.flush_es(0xBD, 0x00);
            if (rng.one_in(64)) d.reset_stream();
        }
        (void)d.flush_es(0xE0, 0);
        (void)d.disable_es(0xBD, 0x40);
        return good && d.units > 0;
    };
    fuzz::run(tally, {{"program stream", stream, program_fields(stream)}}, 1500,
        [&](const std::vector<u8>& input, fuzz::Rng& rng) { return demux(input, rng, false); });
    fuzz::run(tally, {{"raw avc", es, {}}}, 500,
        [&](const std::vector<u8>& input, fuzz::Rng& rng) { return demux(input, rng, true); });
}

// ---- cellAtrac: a game's AT3 and AT3+ files, their RIFF WAVE headers and the frames they lead to

namespace {

// The fields of an AT3+ file: RIFF's size and each chunk's, fmt's tag, channels, rate, block align and configuration,
// fact's samples and first sample's offset, smpl's loop count and its loop's points (little-endian).
std::vector<fuzz::Field> riff_fields(const std::vector<u8>& d)
{
    std::vector<fuzz::Field> f = {{4, 4, true}};
    for (usize at = 12; at + 8 <= d.size();) {
        const u32 len = static_cast<u32>(fuzz::load_field(d, at + 4, 4, true));
        f.push_back({at + 4, 4, true});
        const std::string_view id(reinterpret_cast<const char*>(d.data() + at), 4);
        if (id == "fmt ")
            fuzz::append(f, {{at + 8, 2, true}, {at + 10, 2, true}, {at + 12, 4, true}, {at + 20, 2, true},
                                {at + 50, 2, true}, {at + 52, 1}, {at + 53, 1}});
        if (id == "fact") fuzz::append(f, {{at + 8, 4, true}, {at + 12, 4, true}});
        if (id == "smpl") fuzz::append(f, {{at + 36, 4, true}, {at + 52, 4, true}, {at + 56, 4, true}});
        if (id == "data" || len > d.size()) break;
        at += 8 + len + (len & 1);
    }
    return f;
}

} // namespace

TEST_CASE("fuzz: cellAtrac over damaged AT3+ files, each file flush against unmapped memory", "[hle][fuzz]")
{
#if !PSPOCKET_HAVE_FFMPEG
    SKIP("no FFmpeg in this build (scripts/build-ffmpeg.sh): pspocket's cellAdec has no ATRAC3plus core (D-606)");
#endif
    namespace at3 = pspocket::hle::test::atrac;
    InterpretMisses interpret; // cellAdec's callbacks
    Fixture f;
    add_guarded_areas(f);
    REQUIRE(f.hle->link_internal_functions({"cellAdec", "cellAtrac"}));
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    constexpr u32 kHandle = kData + 0x200, kInfo = kData + 0x500, kWork = kData + 0x100000, kWorkSize = 0x100000;
    const auto call = [&](const char* name, std::initializer_list<u64> args) {
        const hle::Function& fn = GuestThread::find(name);
        u32 r = 3;
        for (const u64 a : args) t.ctx.gpr[r++] = a;
        fn.fn(t.ctx, b);
        return static_cast<u32>(t.ctx.gpr[3]);
    };
    std::vector<fuzz::Sample> samples;
    const std::pair<const char*, at3::FileSpec> specs[] = {{"plain", {.frames = 12}},
        {"fact with an offset", {.frames = 12, .samples = 15000, .offset = 1000}},
        {"fact of 4 bytes", {.frames = 12, .samples = 15000}},
        {"looped", {.frames = 16, .samples = 25000, .loop_start = 5000, .loop_end = 20000}},
        {"odd chunk", {.frames = 8, .odd_chunk = true}}, {"44.1 kHz", {.frames = 8, .rate = 44100}}};
    for (const auto& [name, spec] : specs) {
        std::vector<u8> file = at3::at3p_file(spec);
        auto fields = riff_fields(file);
        samples.push_back({name, std::move(file), std::move(fields)});
    }
    fuzz::Tally tally("cellAtrac");
    u64 decoded = 0;
    fuzz::run(tally, samples, 300, [&](const std::vector<u8>& input, fuzz::Rng& rng) {
        // The whole file read into a buffer of its size, which ends where guest memory does.
        const u32 at = place_input(b, input);
        const u32 size = static_cast<u32>(input.size());
        std::memset(b + kHandle, 0, 0x200);
        if (call("cellAtracSetDataAndGetMemSize", {kHandle, at, size, size, kInfo}) != 0) return false;
        // The decoder (cellAdec's ATRAC3plus core, on FFmpeg) on the work memory the library asked for, most times.
        const u32 work = ps_read32(b, kInfo);
        const bool decoder = work <= kWorkSize && !rng.one_in(4) &&
            call("cellAtracCreateDecoder", {kHandle, kWork, 1000, 100}) == 0;
        ps_write32(b, kInfo, 0);
        call("cellAtracGetChannel", {kHandle, kInfo});
        const u32 channels = ps_read32(b, kInfo);
        ps_write32(b, kInfo, 0);
        call("cellAtracGetMaxSample", {kHandle, kInfo});
        const u32 max_sample = ps_read32(b, kInfo);
        for (const char* q : {"cellAtracGetSamplingRate", "cellAtracGetBitrate", "cellAtracGetNextSample",
                 "cellAtracGetRemainFrame", "cellAtracGetVacantSize", "cellAtracGetNextDecodePosition",
                 "cellAtracGetInternalErrorInfo"})
            call(q, {kHandle, kInfo});
        call("cellAtracGetSoundInfo", {kHandle, kInfo, kInfo + 4, kInfo + 8});
        call("cellAtracGetStreamDataInfo", {kHandle, kInfo, kInfo + 4, kInfo + 8});
        call("cellAtracGetSecondBufferInfo", {kHandle, kInfo, kInfo + 4});
        call("cellAtracGetLoopInfo", {kHandle, kInfo, kInfo + 4});
        call("cellAtracGetBufferInfoForResetting", {kHandle, rng.below(0x8000), kInfo + 0x10});
        if (!decoder) return true;
        // The frames decoded into the buffer a game makes from what the library says a frame gives (its samples,
        // channels, 4-byte floats), which ends where guest memory does too.
        call("cellAtracSetLoopNum", {kHandle, rng.below(3)});
        const u64 pcm_size = u64{max_sample} * channels * 4;
        if (pcm_size && pcm_size <= kAreaSize) {
            const u32 pcm = place_output(b, static_cast<u32>(pcm_size));
            for (int n = 0; n < 6; n++) {
                const u32 r = call("cellAtracDecode", {kHandle, pcm, kInfo, kInfo + 4, kInfo + 8});
                decoded += r == 0;
                if (r != 0 || ps_read32(b, kInfo + 4)) break;
            }
        }
        call("cellAtracDeleteDecoder", {kHandle});
        return true;
    }, kAreaSize);
    std::printf("fuzz %-24s %8llu frames decoded\n", tally.name().c_str(), static_cast<unsigned long long>(decoded));
}

#if PSPOCKET_HAVE_FFMPEG

namespace {

namespace vt = pspocket::hle::test::vdec_test;
namespace h264 = pspocket::hle::test::h264;
namespace vc1 = pspocket::hle::test::vc1;

constexpr u32 CODEC_AVC = 1, CODEC_VC1 = 3, CODEC_MVC = 11;
constexpr u32 kBigPicture = 0x02000000, kBigPictureSize = 0x01000000; // pictures larger than the guarded area

// A decoder opened once; each input is one sequence: a valid access unit, the damaged one, another valid one.
struct VdecRun {
    vt::InterpretMisses interpret;
    vt::BigFixture f;
    GuestThread t{f, 0, "ppu"};
    u8* b = f.base();
    vt::Call call = vt::hle_calls(f, t);
    u32 handle = 0;
    u64 timeouts = 0;

    VdecRun(u32 codec, u32 level)
    {
        add_guarded_areas(f);
        REQUIRE(f.hle->main_memory().reserve(kBigPicture, kBigPictureSize));
        REQUIRE(f.mem.commit(kBigPicture, kBigPictureSize));
        vt::write_callback(b);
        std::memset(b + vt::kArgs, 0, 0x200);
        ps_write32(b, vt::kType, codec);
        ps_write32(b, vt::kType + 4, level);
        REQUIRE(call("cellVdecQueryAttrEx", 0xC982A84A, {vt::kType, vt::kAttr}) == 0);
        const u32 res[6] = {vt::kDecoderMem, std::min(ps_read32(b, vt::kAttr), vt::kDecoderMemSize), 1000, 0x4000, 200, 1};
        for (u32 i = 0; i < 6; i++) ps_write32(b, vt::kRes + 4 * i, res[i]);
        ps_write32(b, vt::kCb, vt::kOpd);
        ps_write32(b, vt::kCb + 4, vt::kLog);
        REQUIRE(call("cellVdecOpenEx", 0x0053E2D8, {vt::kType, vt::kRes, vt::kCb, vt::kHandle}) == 0);
        handle = ps_read32(b, vt::kHandle);
    }
    ~VdecRun() { call("cellVdecClose", 0x16698E83, {handle}); }

    bool wait(u32 type, u32 count)
    {
        if (vt::wait_until([&] { return vt::count_of(b, type) >= count; }, std::chrono::seconds(20))) return true;
        timeouts++;
        return false;
    }

    // One sequence over `aus`: each decoded and awaited, the end of the sequence, every picture taken. Whether every
    // access unit was accepted.
    bool sequence(const std::vector<std::vector<u8>>& aus, fuzz::Rng& rng)
    {
        std::memset(b + vt::kLog, 0, 0x1000); // the decoder is idle between sequences
        if (call("cellVdecStartSeq", 0xC757C2AA, {handle}) != 0) return false;
        bool accepted = true;
        u32 done = 0;
        for (usize i = 0; i < aus.size(); i++) {
            const u32 at = place_input(b, aus[i]);
            ps_write32(b, vt::kAu + 0, at);
            ps_write32(b, vt::kAu + 4, static_cast<u32>(aus[i].size()));
            ps_write32(b, vt::kAu + 8, 0);
            ps_write32(b, vt::kAu + 12, 0x1000 + 3003 * static_cast<u32>(i));
            ps_write32(b, vt::kAu + 16, 0);
            ps_write32(b, vt::kAu + 20, 0x800 + 3003 * static_cast<u32>(i));
            ps_write64(b, vt::kAu + 24, i);
            ps_write64(b, vt::kAu + 32, 0);
            const s32 mode = rng.one_in(8) ? static_cast<s32>(rng.below(3)) : 0;
            if (call("cellVdecDecodeAu", 0x2BF4DDD2, {handle, static_cast<u64>(static_cast<u32>(mode)), vt::kAu}) != 0) {
                accepted = false;
                continue;
            }
            CHECK(wait(0, ++done)); // the decoder took it: its memory is free for the next
        }
        call("cellVdecEndSeq", 0x824433F0, {handle});
        CHECK(wait(2, 1));
        for (int k = 0; k < 64; k++) {
            ps_write32(b, vt::kItemOut, 0);
            if (call("cellVdecGetPicItem", 0x17C702B9, {handle, vt::kItemOut}) != 0) break;
            const u32 item = ps_read32(b, vt::kItemOut);
            const u32 size = ps_read32(b, item + 8);
            ps_write32(b, vt::kFormat, static_cast<u32>(rng.below(4))); // ARGB, RGBA, UYVY, YUV 4:2:0
            ps_write32(b, vt::kFormat + 4, static_cast<u32>(rng.below(2)));
            ps_write8(b, vt::kFormat + 8, 0xFF);
            // The picture flush against unmapped memory when it fits there; its buffer is what libvdec's size says.
            const u32 out = size <= kAreaSize ? place_output(b, size) : size <= kBigPictureSize ? kBigPicture : 0;
            if (call("cellVdecGetPicture", 0x807C861A, {handle, vt::kFormat, out}) != 0)
                call("cellVdecGetPicture", 0x807C861A, {handle, vt::kFormat, 0});
        }
        return accepted;
    }
};

} // namespace

TEST_CASE("fuzz: cellVdec's H.264, MVC and VC-1 stream parsing over damaged access units", "[hle][fuzz]")
{
    // H.264: Baseline IDR access units (AUD, SPS, PPS, an I_PCM slice); the SPS fields reach the parser too.
    const std::vector<u8> avc0 = h264::access_unit(h264::pattern(64, 64, 0), 0),
                          avc1 = h264::access_unit(h264::pattern(64, 64, 1), 1);
    // MVC: two views in one access unit (prefix, subset SPS and slice extension NAL units around the base view's).
    const std::vector<u8> mvc0 = h264::mvc_access_unit(h264::pattern(64, 64, 0), h264::pattern(64, 64, 5), 0),
                          mvc1 = h264::mvc_access_unit(h264::pattern(64, 64, 1), h264::pattern(64, 64, 6), 1);
    // VC-1's advanced profile: a sequence header, an entry point and an intra frame; skipped frames.
    vc1::Sequence seq;
    seq.display_width = 64;
    seq.display_height = 48;
    seq.aspect_ratio = 15;
    seq.aspect_width = 4;
    seq.aspect_height = 3;
    seq.framerate_nr = 2;
    seq.framerate_dr = 2;
    seq.pulldown = true;
    seq.tfcntrflag = true;
    vc1::Entry entry;
    entry.range_mapy = 3;
    vc1::DcPicture dc(64, 48);
    for (usize i = 0; i < dc.y.size(); i++) dc.y[i] = static_cast<int>(i % 9) - 4;
    std::vector<u8> vc1_intra = vc1::sequence_header(seq);
    for (const auto& part : {vc1::entry_point(entry), vc1::intra_frame(dc, seq)}) vc1_intra.insert(vc1_intra.end(), part.begin(), part.end());
    const std::vector<u8> vc1_skipped = vc1::skipped_frame(seq);

    struct Target {
        std::string name;
        u32 codec, level;
        std::vector<u8> first, sample, last;
    };
    const std::vector<Target> targets = {{"vdec avc", CODEC_AVC, 21, avc0, avc1, avc0},
        {"vdec mvc", CODEC_MVC, 21, mvc0, mvc1, mvc0}, {"vdec vc-1", CODEC_VC1, 6, vc1_intra, vc1_intra, vc1_skipped}};
    for (const Target& target : targets) {
        VdecRun run(target.codec, target.level);
        fuzz::Tally tally(target.name);
        fuzz::run(tally, {{target.name, target.sample, {}}}, 150, [&](const std::vector<u8>& input, fuzz::Rng& rng) {
            if (input.empty()) return false; // a size of 0 is refused by DecodeAu's own check
            return run.sequence({target.first, input, target.last}, rng);
        }, kAreaSize);
        CHECK(run.timeouts == 0);
    }
}

// ---- the audio cores on FFmpeg: their own frame parsing (MPEG audio headers and layer III's side information, ADTS
// and the AAC raw blocks' configuration, AC-3's sync information and bit stream information) in front of FFmpeg's
// decoders, each damaged access unit decoded alone (Codec::decode, as the core's thread calls it).

namespace {

namespace ff = pspocket::hle::adec::ffcore;

// MPEG audio frames of silence built here: every allocation and spectral value 0, the frame's size from its header.
std::vector<u8> mpeg_silence(u32 header, u32 bytes)
{
    std::vector<u8> f(bytes, 0);
    ps_st32(f.data(), header);
    return f;
}

// Packets of FFmpeg's encoder `name` over a second of two tones: none when this FFmpeg has no such encoder.
std::vector<std::vector<u8>> encoded(const char* name, u32 rate, u32 channels, s64 bit_rate)
{
    std::vector<std::vector<u8>> packets;
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    if (!codec) return packets;
    AVCodecContext* av = avcodec_alloc_context3(codec);
    av->sample_rate = static_cast<int>(rate);
    av_channel_layout_default(&av->ch_layout, static_cast<int>(channels));
    av->bit_rate = bit_rate;
    const void* formats = nullptr;
    int count = 0;
    REQUIRE(avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &formats, &count) >= 0);
    bool int16 = false;
    for (int i = 0; i < count; i++) int16 |= static_cast<const AVSampleFormat*>(formats)[i] == AV_SAMPLE_FMT_S16;
    av->sample_fmt = int16 ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_FLTP;
    REQUIRE(avcodec_open2(av, codec, nullptr) == 0);
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    const u32 n = static_cast<u32>(av->frame_size);
    for (u32 at = 0; at + n <= rate / 4; at += n) {
        frame->nb_samples = static_cast<int>(n);
        frame->format = av->sample_fmt;
        frame->sample_rate = av->sample_rate;
        REQUIRE(av_channel_layout_copy(&frame->ch_layout, &av->ch_layout) == 0);
        REQUIRE(av_frame_get_buffer(frame, 0) == 0);
        for (u32 i = 0; i < n; i++)
            for (u32 c = 0; c < channels; c++) {
                const float v = 0.3f * std::sin(static_cast<float>(at + i) * (0.03f + 0.02f * static_cast<float>(c)));
                if (int16) reinterpret_cast<s16*>(frame->data[0])[i * channels + c] = static_cast<s16>(v * 32767);
                else reinterpret_cast<float*>(frame->data[c])[i] = v;
            }
        REQUIRE(avcodec_send_frame(av, frame) == 0);
        av_frame_unref(frame);
        while (avcodec_receive_packet(av, pkt) == 0) {
            packets.emplace_back(pkt->data, pkt->data + pkt->size);
            av_packet_unref(pkt);
        }
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&av);
    return packets;
}

// An ADTS frame (MPEG-4, no CRC) around an AAC LC raw frame at 48 kHz in two channels.
std::vector<u8> adts_lc_48k_stereo(const std::vector<u8>& raw)
{
    const u32 length = static_cast<u32>(raw.size()) + 7;
    std::vector<u8> f = {0xFF, 0xF1, static_cast<u8>(1u << 6 | 3u << 2 | 0), static_cast<u8>(2u << 6 | length >> 11),
        static_cast<u8>(length >> 3), static_cast<u8>((length & 7) << 5 | 0x1F), 0xFC};
    f.insert(f.end(), raw.begin(), raw.end());
    return f;
}

std::vector<u8> be_words(std::initializer_list<u32> words)
{
    std::vector<u8> v;
    for (const u32 w : words)
        for (int k = 3; k >= 0; k--) v.push_back(static_cast<u8>(w >> (8 * k)));
    return v;
}

// CellAdecParamAc3 for two channels out: 16-bit, 2/0, no LFE, full DRC and scale, slots 0 and 1.
std::vector<u8> ac3_stereo_param()
{
    std::vector<u8> v(0x30, 0);
    v[0] = 0;
    v[1] = 2;
    v[2] = 0;
    for (const usize at : {usize{4}, usize{8}, usize{0x14}}) ps_st32(v.data() + at, 0x3F800000); // 1.0f
    v[0xC] = 2;
    v[0xD] = 2;
    const s32 pointers[6] = {0, 1, -1, -1, -1, -1};
    for (u32 k = 0; k < 6; k++) ps_st32(v.data() + 0x18 + 4 * k, static_cast<u32>(pointers[k]));
    return v;
}

} // namespace

TEST_CASE("fuzz: the audio cores' frame parsing in front of FFmpeg over damaged access units", "[hle][fuzz]")
{
    Fixture f;
    add_guarded_areas(f);
    u8* b = f.base();
    constexpr u32 kBsi = kData + 0x30000;
    struct Target {
        s32 type;
        std::string name;
        std::vector<u8> param;
        std::vector<std::vector<u8>> units;
    };
    std::vector<Target> targets;
    // MP3: MPEG-1 at 44.1 kHz, 128 kbit/s, stereo (417 bytes); MPEG-2 at 24 kHz, 64 kbit/s, mono (192 bytes).
    targets.push_back({4, "adec mp3", be_words({3}), {mpeg_silence(0xFFFB9004, 417), mpeg_silence(0xFFF384C4, 192)}});
    targets.push_back({4, "adec mp3 float", be_words({4}), {mpeg_silence(0xFFFB9004, 417)}});
    // Layer I at 48 kHz, 256 kbit/s (256 bytes); layer II at 48 kHz, 128 kbit/s (384 bytes), and FFmpeg's mp2.
    targets.push_back({22, "adec mpeg layer 1", be_words({0, 0, 0}), {mpeg_silence(0xFFFF8404, 256)}});
    std::vector<std::vector<u8>> l2 = {mpeg_silence(0xFFFD8404, 384)};
    for (auto& p : encoded("mp2", 48000, 2, 192000)) l2.push_back(std::move(p));
    l2.resize(std::min<usize>(l2.size(), 3));
    targets.push_back({6, "adec mpeg layer 2", be_words({3, 0, 0}), l2});
    // AAC LC from FFmpeg's encoder: ADTS frames, and its raw blocks (configuration 2: rate index 3, LC).
    std::vector<std::vector<u8>> aac = encoded("aac", 48000, 2, 128000);
    if (aac.size() > 3) {
        std::vector<std::vector<u8>> adts_units, raw_units;
        for (usize i = 1; i < 3; i++) { // past the encoder's first, which carries its priming
            adts_units.push_back(adts_lc_48k_stereo(aac[i]));
            raw_units.push_back(aac[i]);
        }
        targets.push_back({16, "adec aac adts", be_words({1, 0, 0, 0}), adts_units});
        targets.push_back({20, "adec aac 2ch raw", be_words({2, 3, 1, 0}), raw_units});
    }
    // AC-3 from FFmpeg's encoder, two channels out.
    std::vector<std::vector<u8>> ac3 = encoded("ac3", 48000, 2, 192000);
    ac3.resize(std::min<usize>(ac3.size(), 2));
    if (!ac3.empty()) targets.push_back({2, "adec ac3", ac3_stereo_param(), ac3});

    for (const Target& target : targets) {
        const ff::Spec* spec = ff::spec_of(target.type);
        REQUIRE(spec);
        REQUIRE(target.param.size() <= spec->param_size + 0x20);
        std::vector<u8> param(std::max<usize>(spec->param_size, target.param.size()), 0);
        std::copy(target.param.begin(), target.param.end(), param.begin());
        REQUIRE(spec->check(param.data()) == lv2::CELL_OK);
        std::unique_ptr<ff::Codec> codec = spec->make();
        REQUIRE(codec->start(param.data()) == lv2::CELL_OK);
        std::vector<fuzz::Sample> samples;
        for (usize i = 0; i < target.units.size(); i++)
            samples.push_back({std::format("{} unit {}", target.name, i), target.units[i], {{0, 4}, {4, 4}}});
        fuzz::Tally tally(target.name);
        u64 decoded = 0;
        fuzz::run(tally, samples, 600, [&](const std::vector<u8>& input, fuzz::Rng& rng) {
            const u32 au = place_input(b, input);
            const u32 out = place_output(b, spec->output_bytes);
            std::memset(b + kBsi, 0, spec->bsi_size);
            const ff::Decoded d = codec->decode(b, au, static_cast<u32>(input.size()), out, kBsi);
            CHECK(d.size <= spec->output_bytes);
            if (rng.one_in(64)) REQUIRE(codec->start(param.data()) == lv2::CELL_OK); // a new sequence now and then
            decoded += d.size > 0;
            return d.error == 0;
        }, kAreaSize);
        CHECK(decoded > 0);
    }
}

#endif
