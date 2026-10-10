// cellVdec's newest codecs against Sony's libvdec (D-588 to D-590, D-597): DivX 3.11, MPEG-4 part 2 (as DivX and as
// MPEG-4), VC-1's advanced profile and MVC, the streams test_vdec_codecs.cpp decodes (vdec_streams.hpp,
// vc1_writer.hpp, h264_writer.hpp), each family in a process of its own per half. Sony's libvdec loads Sony's codec
// cores (libdivx311dec, libdivxdec, libsmvd4, libsvc1d, libmvcdec: firmware-first or Sony's only, D-575). A decoder's
// life is decode_stream's (vdec_test_util.hpp) written as observations: QueryAttrEx's attribute (with and without the
// codec specific information), every answer, the callbacks in order, the pictures announced before EndSeq (the lag),
// and for each picture its item (the frame buffer's address as an offset into the decoder's memory), its information
// block (the codec's size by RPCS3's attributes: DivX 0x20, DivX 3.11 0x18, MPEG-4 0x38, VC-1 0x170, MVC 0x214), its
// frame buffer (Sony's layout as cellVdec.cpp knows it from H.264) and its samples.
//
// What counts as the same picture (D-597): VC-1's transforms are exact by the standard and the MVC streams are I_PCM,
// so those pictures and frame buffers must be the same sample for sample. DivX and MPEG-4 part 2 use an IDCT the
// standard lets decoders round in their own way (IEEE 1180), so their samples may be one step off FFmpeg's; the counts
// within the tolerance are reported. Skipped without the firmware (compare_util.hpp), and the DivX and VC-1 parts
// where this FFmpeg lacks the encoders or the decoder (build-ffmpeg.sh). No game data.

#include <catch_amalgamated.hpp>

#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include "compare_util.hpp"
#include "h264_writer.hpp"
#include "vc1_writer.hpp"
#include "vdec_streams.hpp"
#include "vdec_test_util.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;
using namespace pspocket::hle::test::vdec_test;
namespace v1 = pspocket::hle::test::vc1;
namespace h = pspocket::hle::test::h264;

TEST_CASE("cellVdec's comparison calls Sony's exports by their names; pspocket's library", "[hle][vdec]")
{
    // libvdec is pspocket's own and compared already (D-196): not firmware-first, only its new codecs are new here.
    check_names("libvdec", {"cellVdecQueryAttrEx", "cellVdecOpenEx", "cellVdecSetFrameRate", "cellVdecStartSeq",
                               "cellVdecDecodeAu", "cellVdecEndSeq", "cellVdecGetPicItem", "cellVdecGetPicture",
                               "cellVdecClose"},
        false);
    CHECK_FALSE(hle::registry().is_firmware_first("libvdec"));
}

#if PSPOCKET_HAVE_FFMPEG
using namespace pspocket::hle::test::vdec_streams;

namespace {

constexpr u32 CODEC_AVC = 1, CODEC_MPEG4 = 2, CODEC_VC1 = 3, CODEC_DIVX = 5, CODEC_DIVX3_11 = 9, CODEC_MVC = 11,
              CODEC_MVC2 = 13;

// Sony's information block of each codec (RPCS3's VDEC_SCE_DECODER attributes).
u32 info_size(u32 codec)
{
    switch (codec) {
    case CODEC_MPEG4: return 0x38;
    case CODEC_DIVX: return 0x20;
    case CODEC_DIVX3_11: return 0x18;
    case CODEC_VC1: return 0x170;
    case CODEC_MVC:
    case CODEC_MVC2: return 0x214;
    default: return 0x244;
    }
}

// Sony's frame buffer of a picture (cellVdec.cpp's frame_size, from H.264): 16-byte strips of luma, then of chroma,
// two strips of repeated edge on each side.
u32 frame_size(u32 width, u32 height)
{
    const u32 strips = (width + 15) / 16 + 4;
    return strips * 16 * height + 0x400 + strips * 16 * (height / 2) + 0x400;
}

std::string in_decoder(u32 addr)
{
    if (addr >= kDecoderMem && addr < kDecoderMem + kDecoderMemSize)
        return std::format("the decoder's memory +{:#x}", addr - kDecoderMem);
    return std::format("{:#x}", addr);
}

using Run = Half<BigFixture>;

// QueryAttrEx of a codec, level and specific information: its answer and attribute.
void query(Run& r, const std::string& what, u32 codec, u32 level, const std::vector<u8>& spec)
{
    u8* b = r.b;
    std::memset(b + kArgs, 0, 0x100);
    ps_write32(b, kType, codec);
    ps_write32(b, kType + 4, level);
    if (!spec.empty()) {
        std::memcpy(b + kSpec, spec.data(), spec.size());
        ps_write32(b, kType + 8, kSpec);
    }
    std::memset(b + kAttr, 0xEE, 16);
    r.r("cellVdecQueryAttrEx", {kType, kAttr}, "QueryAttrEx " + what);
    r.o.line("QueryAttrEx " + what + ": the attribute", compare::hex(b + kAttr, 16));
}

// A specific information block {thisSize, maxDecodedFrameWidth, maxDecodedFrameHeight} (MPEG-4, VC-1, DivX's first
// form) or H.264's {12, width, height, deblocking, buffers}.
std::vector<u8> spec8(u32 size, u16 width, u16 height)
{
    std::vector<u8> v = {static_cast<u8>(size >> 24), static_cast<u8>(size >> 16), static_cast<u8>(size >> 8),
        static_cast<u8>(size), static_cast<u8>(width >> 8), static_cast<u8>(width), static_cast<u8>(height >> 8),
        static_cast<u8>(height)};
    v.resize(size == 12 ? 12 : 8, 0);
    return v;
}

// A decoder's life over `s` (decode_stream's), as observations keyed by `what`.
void decode(Run& r, const std::string& what, const Stream& s, u32 width, u32 height, Rule rule)
{
    u8* b = r.b;
    write_callback(b);
    std::memset(b + kArgs, 0, 0x200);
    ps_write32(b, kType, s.codec);
    ps_write32(b, kType + 4, s.level);
    if (!s.spec.empty()) {
        std::memcpy(b + kSpec, s.spec.data(), s.spec.size());
        ps_write32(b, kType + 8, kSpec);
    }
    std::memset(b + kAttr, 0xEE, 16);
    if (r.r("cellVdecQueryAttrEx", {kType, kAttr}, what + ": QueryAttrEx") != 0) return;
    r.o.line(what + ": QueryAttrEx's attribute", compare::hex(b + kAttr, 16));
    const u32 res[6] = {kDecoderMem, std::min(ps_read32(b, kAttr), kDecoderMemSize), 1000, 0x4000, 200, 1};
    for (u32 i = 0; i < 6; i++) ps_write32(b, kRes + 4 * i, res[i]);
    ps_write32(b, kCb, kOpd);
    ps_write32(b, kCb + 4, kLog);
    ps_write32(b, kHandle, 0xEEEEEEEE);
    if (r.r("cellVdecOpenEx", {kType, kRes, kCb, kHandle}, what + ": OpenEx") != 0) return;
    const u32 handle = ps_read32(b, kHandle);
    r.o.line(what + ": the handle", in_decoder(handle));
    if (s.frame_rate) r.r("cellVdecSetFrameRate", {handle, s.frame_rate}, what + ": SetFrameRate");
    if (r.r("cellVdecStartSeq", {handle}, what + ": StartSeq") == 0) {
        u32 at = kStream, accepted = 0;
        for (usize i = 0; i < s.aus.size(); i++) {
            const std::vector<u8>& au = s.aus[i];
            REQUIRE(at + au.size() <= kPicture);
            std::memcpy(b + at, au.data(), au.size());
            ps_write32(b, kAu + 0, at);
            ps_write32(b, kAu + 4, static_cast<u32>(au.size()));
            ps_write32(b, kAu + 8, 0);
            ps_write32(b, kAu + 12, 0x1000 + 3003 * static_cast<u32>(i));
            ps_write32(b, kAu + 16, 0);
            ps_write32(b, kAu + 20, 0x800 + 3003 * static_cast<u32>(i));
            ps_write64(b, kAu + 24, 0xABCD0000 + static_cast<u64>(i));
            ps_write64(b, kAu + 32, 0);
            at = (at + static_cast<u32>(au.size()) + 15) & ~15u;
            const std::string key = std::format("{}: DecodeAu {}", what, i);
            if (r.r("cellVdecDecodeAu", {handle, static_cast<u64>(static_cast<u32>(s.mode)), kAu}, key) != 0) continue;
            accepted++;
            if (!wait_until([&] { return count_of(b, 0) >= accepted; })) r.o.line(key + ": AUDONE", "none within 60 s");
        }
        // The pictures announced before the end of the sequence (once no more come for 0.4 s): the lag.
        u32 seen = count_of(b, 1);
        for (int quiet = 0; quiet < 20; quiet++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const u32 now = count_of(b, 1);
            if (now != seen) quiet = 0;
            seen = now;
        }
        r.o.line(what + ": pictures announced before EndSeq", std::format("{}", seen));
        r.r("cellVdecEndSeq", {handle}, what + ": EndSeq");
        r.o.line(what + ": SEQDONE", wait_until([&] { return count_of(b, 2) >= 1; }) ? "yes" : "none within 60 s");
        for (u32 k = 0;; k++) {
            ps_write32(b, kItemOut, 0);
            const std::string key = std::format("{}: picture {}", what, k);
            if (r.r("cellVdecGetPicItem", {handle, kItemOut}, key + ": GetPicItem") != 0) break;
            const u32 item = ps_read32(b, kItemOut);
            if (!r.readable(item, 0x4C)) {
                r.o.line(key + ": the item at", std::format("{:#x}, not in guest memory", item));
                break;
            }
            const u32 start = ps_read32(b, item + 4), info = ps_read32(b, item + 0x48);
            std::vector<u8> fields(b + item, b + item + 0x4C);
            std::memset(fields.data() + 4, 0, 4);  // the frame buffer's address: below
            std::memset(fields.data() + 0x48, 0, 4); // the information's: below
            r.o.line(key + ": the item at", in_decoder(item));
            r.o.line(key + ": the item", compare::hex(fields));
            r.o.line(key + ": its frame buffer at", in_decoder(start));
            r.o.line(key + ": its information at the item", std::format("{:+#x}", static_cast<s64>(info) - item));
            const u32 info_bytes = info_size(s.codec);
            r.o.line(key + ": the information",
                r.readable(info, info_bytes) ? compare::hex(b + info, info_bytes) : std::string("not in guest memory"));
            if (start >= kDecoderMem && start + u64{frame_size(width, height)} <= kDecoderMem + kDecoderMemSize)
                r.o.bytes(key + ": its frame buffer", b + start, frame_size(width, height), rule);
            ps_write32(b, kFormat, 3); // YUV 4:2:0 planar
            ps_write32(b, kFormat + 4, 0);
            ps_write8(b, kFormat + 8, 0);
            const u32 size = std::min(ps_read32(b, item + 8), 0x1F0000u); // the item's picture size (kPicture holds 2 MB)
            std::memset(b + kPicture, 0xCC, size + 0x100);
            if (r.r("cellVdecGetPicture", {handle, kFormat, kPicture}, key + ": GetPicture") != 0) break;
            r.o.bytes(key + ": the samples", b + kPicture, size, rule);
            r.o.line(key + ": the 256 bytes past the samples", compare::hex(b + kPicture + size, 256));
        }
        for (u32 i = 0; i < log_count(b) && i < 255; i++)
            r.o.line(std::format("{}: callback {}", what, i),
                std::format("type {} data {:#x} handle {}", ps_read32(b, kLog + 16 + 16 * i),
                    ps_read32(b, kLog + 20 + 16 * i), in_decoder(ps_read32(b, kLog + 24 + 16 * i))));
    }
    r.r("cellVdecClose", {handle}, what + ": Close");
}

Stream stream(u32 codec, u32 level, std::vector<std::vector<u8>> aus, std::vector<u8> spec = {}, u32 frame_rate = 0,
    s32 mode = 0)
{
    Stream s;
    s.codec = codec;
    s.level = level;
    s.aus = std::move(aus);
    s.spec = std::move(spec);
    s.frame_rate = frame_rate;
    s.mode = mode;
    return s;
}

constexpr const char* kNoDivx = "this FFmpeg has no msmpeg4v3 or mpeg4 encoder (the tests' build: build-ffmpeg.sh)";
constexpr const char* kNoVc1 = "this FFmpeg has no VC-1 decoder (build-ffmpeg.sh again)";

Observed divx311(const Setup& s, bool sony_half)
{
    Run r(s, sony_half, "libvdec");
    // The sizes: without the codec specific information, with it, and its checks (RPCS3's query_attr_divx).
    query(r, "DivX 3.11 level 15", CODEC_DIVX3_11, 15, {});
    query(r, "DivX 3.11 level 14", CODEC_DIVX3_11, 14, {});
    query(r, "DivX 3.11 1920x1088, 2 buffers", CODEC_DIVX3_11, 15, divx_info2(12, 1920, 1088, 2));
    query(r, "DivX 3.11 96x64", CODEC_DIVX3_11, 15, divx_info2(12, 96, 64, 0));
    query(r, "DivX 3.11 with DivX's first information form", CODEC_DIVX3_11, 15, divx_info2(8, 96, 64, 0));
    query(r, "DivX 3.11 1921 wide", CODEC_DIVX3_11, 15, divx_info2(12, 1921, 64, 0));
    query(r, "DivX 3.11 1089 high", CODEC_DIVX3_11, 15, divx_info2(12, 96, 1089, 0));
    query(r, "DivX 3.11 1 buffer", CODEC_DIVX3_11, 15, divx_info2(12, 96, 64, 1));
    const std::vector<std::vector<u8>> packets = encode(AV_CODEC_ID_MSMPEG4V3, 96, 64, 6, 3);
    const Rule rule = Rule::within(1);
    decode(r, "DivX 3.11 96x64, six pictures", stream(CODEC_DIVX3_11, 15, packets, divx_info2(12, 96, 64, 0)), 96, 64,
        rule);
    decode(r, "DivX 3.11 96x64, SetFrameRate 25", stream(CODEC_DIVX3_11, 15, packets, divx_info2(12, 96, 64, 0), 0x82),
        96, 64, rule);
    // The frame size given smaller and larger than the stream's (where libdivx311dec takes it from).
    decode(r, "DivX 3.11 96x64 told 64x48", stream(CODEC_DIVX3_11, 15, packets, divx_info2(12, 64, 48, 0)), 96, 64, rule);
    decode(r, "DivX 3.11 96x64 told 128x96", stream(CODEC_DIVX3_11, 15, packets, divx_info2(12, 128, 96, 0)), 96, 64,
        rule);
    // Without the information: RPCS3's default frame size (720x576).
    decode(r, "DivX 3.11 720x576 without the information",
        stream(CODEC_DIVX3_11, 15, encode(AV_CODEC_ID_MSMPEG4V3, 720, 576, 1, 3)), 720, 576, rule);
    return std::move(r.o);
}

Observed mpeg4(const Setup& s, bool sony_half)
{
    Run r(s, sony_half, "libvdec");
    query(r, "MPEG-4 SP L1", CODEC_MPEG4, 0, {});
    query(r, "MPEG-4 SP L1 96x64", CODEC_MPEG4, 0, spec8(8, 96, 64));
    query(r, "DivX QMOBILE", CODEC_DIVX, 10, {});
    query(r, "DivX QMOBILE 96x64", CODEC_DIVX, 10, spec8(8, 96, 64));
    query(r, "DivX QMOBILE 96x64, 0 buffers", CODEC_DIVX, 10, divx_info2(12, 96, 64, 0));
    for (const AVRational sar : {AVRational{12, 11}, AVRational{8, 9}}) {
        const std::vector<std::vector<u8>> packets =
            encode(AV_CODEC_ID_MPEG4, 96, 64, 4, 2, [&](AVCodecContext* av) { av->sample_aspect_ratio = sar; });
        for (const u32 codec : {CODEC_DIVX, CODEC_MPEG4})
            decode(r, std::format("{} 96x64, pixels {}:{}", codec == CODEC_DIVX ? "DivX" : "MPEG-4", sar.num, sar.den),
                stream(codec, codec == CODEC_DIVX ? 10 : 0, packets), 96, 64, Rule::within(1));
    }
    return std::move(r.o);
}

Observed vc1_family(const Setup& s, bool sony_half)
{
    Run r(s, sony_half, "libvdec");
    for (u32 level = 0; level <= 10; level++) query(r, std::format("VC-1 level {}", level), CODEC_VC1, level, {});
    query(r, "VC-1 AP L1 64x48", CODEC_VC1, 6, spec8(8, 64, 48));
    query(r, "VC-1 AP L4 1920x1088", CODEC_VC1, 9, spec8(8, 1920, 1088));
    // The simple and main profiles: what OpenEx says (pspocket refuses them, FATAL).
    for (u32 level = 0; level < 5; level++) {
        u8* b = r.b;
        write_callback(b);
        std::memset(b + kArgs, 0, 0x200);
        ps_write32(b, kType, CODEC_VC1);
        ps_write32(b, kType + 4, level);
        r.side.call("cellVdecQueryAttrEx", {kType, kAttr});
        const u32 res[6] = {kDecoderMem, std::min(ps_read32(b, kAttr), kDecoderMemSize), 1000, 0x4000, 200, 1};
        for (u32 i = 0; i < 6; i++) ps_write32(b, kRes + 4 * i, res[i]);
        ps_write32(b, kCb, kOpd);
        ps_write32(b, kCb + 4, kLog);
        if (r.r("cellVdecOpenEx", {kType, kRes, kCb, kHandle}, std::format("OpenEx VC-1 level {}", level)) == 0)
            r.r("cellVdecClose", {ps_read32(b, kHandle)}, std::format("Close VC-1 level {}", level));
    }
    // test_vdec_codecs.cpp's streams: I, skipped, I, skipped with a display extension; range mapping and pulldown;
    // access units before the headers; a new sequence header; the decode modes.
    v1::Sequence seq;
    seq.display_width = 64;
    seq.display_height = 48;
    seq.aspect_ratio = 15;
    seq.aspect_width = 4;
    seq.aspect_height = 3;
    seq.framerate_nr = 2;
    seq.framerate_dr = 1;
    seq.color_prim = 1;
    seq.transfer_char = 1;
    seq.matrix_coef = 6;
    const v1::Entry entry;
    const v1::DcPicture a = dc_levels(64, 48, 0), c = dc_levels(64, 48, 1);
    const std::vector<u8> headers = concat({v1::sequence_header(seq), v1::entry_point(entry)});
    decode(r, "VC-1 64x48 I, skipped, I, skipped",
        stream(CODEC_VC1, 6,
            {concat({headers, v1::intra_frame(a, seq)}), v1::skipped_frame(seq), concat({headers, v1::intra_frame(c, seq)}),
                v1::skipped_frame(seq)}),
        64, 48, Rule::exact());
    {
        v1::Sequence pull;
        pull.pulldown = true;
        pull.tfcntrflag = true;
        v1::Entry mapped;
        mapped.range_mapy = 7;
        mapped.range_mapuv = 1;
        const std::vector<u8> hd = concat({v1::sequence_header(pull), v1::entry_point(mapped)});
        decode(r, "VC-1 64x48 with range mapping and pulldown",
            stream(CODEC_VC1, 9, {concat({hd, v1::intra_frame(dc_levels(64, 48, 2), pull, 2)})}), 64, 48, Rule::exact());
    }
    const v1::Sequence plain;
    const std::vector<u8> plain_headers = concat({v1::sequence_header(plain), v1::entry_point(entry)});
    const v1::DcPicture d = dc_levels(64, 48, 3);
    decode(r, "VC-1 64x48 with an access unit before the headers",
        stream(CODEC_VC1, 5,
            {v1::skipped_frame(plain), concat({plain_headers, v1::intra_frame(d, plain)}), v1::skipped_frame(plain)}),
        64, 48, Rule::exact());
    {
        v1::Sequence small;
        small.width = small.height = 32;
        // Each picture's size: the stream's first is 64x48, then 32x32 (the frame buffer compared at the first's).
        decode(r, "VC-1 a 64x48 sequence header, then a 32x32 one",
            stream(CODEC_VC1, 5,
                {concat({plain_headers, v1::intra_frame(d, plain)}),
                    concat({v1::sequence_header(small), v1::entry_point(entry),
                        v1::intra_frame(dc_levels(32, 32, 4), small)})}),
            64, 48, Rule::exact());
    }
    decode(r, "VC-1 64x48 in mode 3 (B and PB skipped)",
        stream(CODEC_VC1, 5, {concat({plain_headers, v1::intra_frame(d, plain)})}, {}, 0, 3), 64, 48, Rule::exact());
    decode(r, "VC-1 64x48 in mode 2", stream(CODEC_VC1, 5, {concat({plain_headers, v1::intra_frame(d, plain)})}, {}, 0, 2),
        64, 48, Rule::exact());
    return std::move(r.o);
}

Observed mvc(const Setup& s, bool sony_half)
{
    Run r(s, sony_half, "libvdec");
    for (const u32 codec : {CODEC_MVC, CODEC_MVC2}) {
        query(r, std::format("codec {} level 21", codec), codec, 21, {});
        query(r, std::format("codec {} level 21 64x64", codec), codec, 21, spec8(12, 64, 64));
    }
    std::vector<std::vector<u8>> two_views, apart;
    for (int i = 0; i < 4; i++)
        two_views.push_back(h::mvc_access_unit(h::pattern(64, 64, i), h::pattern(64, 64, 10 + i), static_cast<u32>(i & 1)));
    for (int i = 0; i < 2; i++) {
        apart.push_back(h::access_unit(h::pattern(64, 64, i), static_cast<u32>(i)));
        apart.push_back(h::second_view_only(h::pattern(64, 64, 10 + i), static_cast<u32>(i)));
    }
    for (const u32 codec : {CODEC_MVC, CODEC_MVC2})
        decode(r, std::format("codec {}: four access units of two views", codec), stream(codec, 21, two_views), 64, 64,
            Rule::exact());
    decode(r, "MVC: the views in access units apart", stream(CODEC_MVC, 21, apart), 64, 64, Rule::exact());
    // The same base view as H.264, for reading the MVC differences against (Sony's H.264 is pspocket's, D-196).
    std::vector<std::vector<u8>> base;
    for (int i = 0; i < 4; i++) base.push_back(h::access_unit(h::pattern(64, 64, i), static_cast<u32>(i & 1)));
    decode(r, "H.264: the base view alone", stream(CODEC_AVC, 21, base), 64, 64, Rule::exact());
    return std::move(r.o);
}

} // namespace

TEST_CASE("cellVdec's new codecs against Sony's libvdec: DivX 3.11", "[.][hle][lle][compare][vdec]")
{
    if (!avcodec_find_encoder(AV_CODEC_ID_MSMPEG4V3)) SKIP(kNoDivx);
    const Setup s = setup();
    run(s, "cellVdec's DivX 3.11 against Sony's libvdec", [&](bool sony) { return divx311(s, sony); },
        [](const Observed& o) {
            // pspocket's own half: test_vdec_codecs.cpp's answers.
            CHECK(o.value("QueryAttrEx DivX 3.11 level 15: the attribute").starts_with("hex:001e3578"));
            CHECK(o.value("QueryAttrEx DivX 3.11 level 14") == "0x80610101");
            CHECK(o.value("QueryAttrEx DivX 3.11 1 buffer") == "0x80610101");
            CHECK(o.value("DivX 3.11 96x64, six pictures: pictures announced before EndSeq") == "5");
            CHECK(o.value("DivX 3.11 96x64, six pictures: SEQDONE") == "yes");
            CHECK(o.value("DivX 3.11 96x64, six pictures: picture 5: GetPicture") == "0x0");
            CHECK(o.value("DivX 3.11 96x64, six pictures: picture 6: GetPicItem") != "0x0");
            CHECK(o.value("DivX 3.11 96x64, six pictures: the handle") == "the decoder's memory +0x0");
            CHECK(o.value("DivX 3.11 96x64, SetFrameRate 25: picture 0: the information").starts_with(
                "hex:000000600040"));
            const Data* pic = o.find_data("DivX 3.11 720x576 without the information: picture 0: the samples");
            REQUIRE(pic);
            CHECK(pic->bytes.size() == 720 * 576 * 3 / 2);
        });
}

TEST_CASE("cellVdec's new codecs against Sony's libvdec: MPEG-4 part 2 as DivX and as MPEG-4",
    "[.][hle][lle][compare][vdec]")
{
    if (!avcodec_find_encoder(AV_CODEC_ID_MPEG4)) SKIP(kNoDivx);
    const Setup s = setup();
    run(s, "cellVdec's MPEG-4 part 2 against Sony's libvdec", [&](bool sony) { return mpeg4(s, sony); },
        [](const Observed& o) {
            CHECK(o.value("DivX 96x64, pixels 12:11: pictures announced before EndSeq") == "3");
            // CellVdecDivxInfo: an I picture 96x64, pixel aspect ratio code 2.
            CHECK(o.value("DivX 96x64, pixels 12:11: picture 0: the information").starts_with("hex:000000600040020000"));
            CHECK(o.value("MPEG-4 96x64, pixels 8:9: picture 1: the information").starts_with("hex:0100006000400f0809"));
        });
}

TEST_CASE("cellVdec's new codecs against Sony's libvdec: VC-1's advanced profile", "[.][hle][lle][compare][vdec]")
{
    if (!avcodec_find_decoder(AV_CODEC_ID_VC1)) SKIP(kNoVc1);
    const Setup s = setup();
    run(s, "cellVdec's VC-1 against Sony's libvdec", [&](bool sony) { return vc1_family(s, sony); },
        [](const Observed& o) {
            CHECK(o.value("OpenEx VC-1 level 0") == "0x80610180");
            CHECK(o.value("QueryAttrEx VC-1 level 10") == "0x80610101");
            CHECK(o.value("VC-1 64x48 I, skipped, I, skipped: pictures announced before EndSeq") == "3");
            const Data* pic = o.find_data("VC-1 64x48 I, skipped, I, skipped: picture 0: the samples");
            REQUIRE(pic);
            CHECK(pic->bytes == v1::samples(dc_levels(64, 48, 0)));
            // pspocket's CellVdecVc1Info: 64x48 first.
            CHECK(o.value("VC-1 64x48 I, skipped, I, skipped: picture 0: the information").starts_with("hex:00400030"));
            CHECK(o.value("VC-1 64x48 with an access unit before the headers: picture 0: the item at") !=
                "(missing)");
            CHECK(o.value("VC-1 64x48 in mode 2: DecodeAu 0") == "0x80610101");
        });
}

TEST_CASE("cellVdec's new codecs against Sony's libvdec: MVC", "[.][hle][lle][compare][vdec]")
{
    const Setup s = setup();
    run(s, "cellVdec's MVC against Sony's libvdec", [&](bool sony) { return mvc(s, sony); },
        [](const Observed& o) {
            CHECK(o.value("codec 11: four access units of two views: pictures announced before EndSeq") == "3");
            const Data* pic = o.find_data("codec 11: four access units of two views: picture 2: the samples");
            REQUIRE(pic);
            const h::Picture want = h::pattern(64, 64, 2);
            CHECK(pic->bytes == concat({want.y, want.cb, want.cr}));
            // MVC's information is H.264's from the base view (the first 0x214 bytes of it).
            CHECK(o.value("codec 11: four access units of two views: picture 0: the information") ==
                o.value("H.264: the base view alone: picture 0: the information").substr(0, 4 + 2 * 0x214));
            CHECK(o.value("MVC: the views in access units apart: picture 1: the item") != "(missing)");
        });
}

#else

TEST_CASE("cellVdec's new codecs against Sony's libvdec", "[.][hle][lle][compare][vdec]")
{
    SKIP("no FFmpeg in this build: cellVdec decodes nothing of its own");
}

#endif
