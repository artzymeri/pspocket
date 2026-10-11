// cellVdec's codecs past H.264: DivX 3.11 and MPEG-4 part 2 streams written by FFmpeg's own encoders (the tests'
// build of FFmpeg, D-577, D-588) and VC-1 written here (vc1_writer.hpp, D-589), each decoded through pspocket's libvdec
// and compared with FFmpeg's own decoding of the same packets and with the pictures they hold: the answers, callbacks,
// picture items, information blocks and pixels. No game data.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include "h264_writer.hpp"
#include "vc1_writer.hpp"
#include "vdec_test_util.hpp"

#if PSPOCKET_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}
#endif

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::vdec_test;
namespace v1 = pspocket::hle::test::vc1;
namespace h = pspocket::hle::test::h264;

#if PSPOCKET_HAVE_FFMPEG
namespace {

constexpr u32 CODEC_AVC = 1, CODEC_MPEG4 = 2, CODEC_VC1 = 3, CODEC_DIVX = 5, CODEC_DIVX3_11 = 9, CODEC_MVC = 11,
              CODEC_MVC2 = 13;
constexpr u32 CELL_VDEC_ERROR_ARG = 0x80610101, CELL_VDEC_ERROR_FATAL = 0x80610180;
// A build of FFmpeg from before D-588 (the Mac's until build-ffmpeg.sh runs again) has no VC-1 decoder.
constexpr const char* kNoVc1 = "this FFmpeg has no VC-1 decoder (build-ffmpeg.sh again)";

// A picture in YUV 4:2:0 planes without padding, as cellVdec's GetPicture gives it.
struct Yuv {
    int width = 0, height = 0;
    std::vector<u8> data; // Y, then U, then V

    Yuv(int w, int h) : width(w), height(h), data(static_cast<usize>(w * h * 3 / 2)) {}
    u8& y(int x, int r) { return data[static_cast<usize>(r * width + x)]; }
    u8& u(int x, int r) { return data[static_cast<usize>(width * height + r * (width / 2) + x)]; }
    u8& v(int x, int r) { return data[static_cast<usize>(width * height * 5 / 4 + r * (width / 2) + x)]; }
};

// A smooth picture that moves with `i` (so P pictures have something to predict).
Yuv moving(int width, int height, int i)
{
    Yuv p(width, height);
    for (int r = 0; r < height; r++)
        for (int x = 0; x < width; x++) p.y(x, r) = static_cast<u8>(30 + x + r / 2 + 2 * i);
    for (int r = 0; r < height / 2; r++)
        for (int x = 0; x < width / 2; x++) {
            p.u(x, r) = static_cast<u8>(100 + x / 2 + i);
            p.v(x, r) = static_cast<u8>(150 - r / 2);
        }
    return p;
}

// The planes of a decoded frame, without padding.
Yuv planes(const AVFrame* f)
{
    Yuv p(f->width, f->height);
    for (int r = 0; r < f->height; r++)
        std::memcpy(&p.y(0, r), f->data[0] + r * f->linesize[0], static_cast<usize>(f->width));
    for (int r = 0; r < f->height / 2; r++) {
        std::memcpy(&p.u(0, r), f->data[1] + r * f->linesize[1], static_cast<usize>(f->width / 2));
        std::memcpy(&p.v(0, r), f->data[2] + r * f->linesize[2], static_cast<usize>(f->width / 2));
    }
    return p;
}

// `frames` pictures written by FFmpeg's encoder for `id` at a constant quantiser, 25 a second, an I picture every
// `gop`: one packet each.
std::vector<std::vector<u8>> encode(AVCodecID id, int width, int height, int frames, int gop,
    const std::function<void(AVCodecContext*)>& setup = {})
{
    std::vector<std::vector<u8>> packets;
    const AVCodec* codec = avcodec_find_encoder(id);
    REQUIRE(codec);
    AVCodecContext* av = avcodec_alloc_context3(codec);
    av->width = width;
    av->height = height;
    av->pix_fmt = AV_PIX_FMT_YUV420P;
    av->time_base = {1, 25};
    av->framerate = {25, 1};
    av->gop_size = gop;
    av->max_b_frames = 0;
    av->thread_count = 1;
    av->flags |= AV_CODEC_FLAG_QSCALE;
    av->global_quality = FF_QP2LAMBDA * 2;
    if (setup) setup(av);
    REQUIRE(avcodec_open2(av, codec, nullptr) == 0);
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    const auto drain = [&] {
        while (avcodec_receive_packet(av, pkt) == 0) {
            packets.emplace_back(pkt->data, pkt->data + pkt->size);
            av_packet_unref(pkt);
        }
    };
    for (int i = 0; i < frames; i++) {
        frame->width = width;
        frame->height = height;
        frame->format = AV_PIX_FMT_YUV420P;
        REQUIRE(av_frame_get_buffer(frame, 0) == 0);
        Yuv p = moving(width, height, i);
        for (int r = 0; r < height; r++)
            std::memcpy(frame->data[0] + r * frame->linesize[0], &p.y(0, r), static_cast<usize>(width));
        for (int r = 0; r < height / 2; r++) {
            std::memcpy(frame->data[1] + r * frame->linesize[1], &p.u(0, r), static_cast<usize>(width / 2));
            std::memcpy(frame->data[2] + r * frame->linesize[2], &p.v(0, r), static_cast<usize>(width / 2));
        }
        frame->pts = i;
        frame->quality = av->global_quality;
        REQUIRE(avcodec_send_frame(av, frame) == 0);
        av_frame_unref(frame);
        drain();
    }
    avcodec_send_frame(av, nullptr);
    drain();
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&av);
    return packets;
}

// FFmpeg's own decoding of `packets` by decoder `id` (the frame size given for a codec whose stream has none; the
// configuration for one that needs it).
std::vector<Yuv> ffmpeg_decode(AVCodecID id, const std::vector<std::vector<u8>>& packets, int width = 0, int height = 0,
    const std::vector<u8>& extradata = {})
{
    std::vector<Yuv> out;
    const AVCodec* codec = avcodec_find_decoder(id);
    REQUIRE(codec);
    AVCodecContext* av = avcodec_alloc_context3(codec);
    av->width = av->coded_width = width;
    av->height = av->coded_height = height;
    av->thread_count = 1;
    if (!extradata.empty()) {
        av->extradata = static_cast<u8*>(av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        std::memcpy(av->extradata, extradata.data(), extradata.size());
        av->extradata_size = static_cast<int>(extradata.size());
    }
    REQUIRE(avcodec_open2(av, codec, nullptr) == 0);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const auto drain = [&] {
        while (avcodec_receive_frame(av, frame) == 0) {
            out.push_back(planes(frame));
            av_frame_unref(frame);
        }
    };
    for (const std::vector<u8>& data : packets) {
        std::vector<u8> padded(data);
        padded.resize(data.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        pkt->data = padded.data();
        pkt->size = static_cast<int>(data.size());
        REQUIRE(avcodec_send_packet(av, pkt) == 0);
        drain();
    }
    avcodec_send_packet(av, nullptr);
    drain();
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&av);
    return out;
}

// The luma's peak signal to noise ratio of `got` against `want`, in dB.
double psnr(const Yuv& got, Yuv want)
{
    double se = 0;
    for (int r = 0; r < want.height; r++)
        for (int x = 0; x < want.width; x++) {
            const double d = static_cast<double>(got.data[static_cast<usize>(r * want.width + x)]) - want.y(x, r);
            se += d * d;
        }
    const double mse = se / (want.width * want.height);
    return mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// CellVdecDivxSpecificInfo2 (RPCS3 cellVdec.h): thisSize, maxDecodedFrameWidth, maxDecodedFrameHeight,
// numberOfDecodedFrameBuffer, big-endian.
std::vector<u8> divx_info2(u32 this_size, u16 width, u16 height, u16 buffers)
{
    return {static_cast<u8>(this_size >> 24), static_cast<u8>(this_size >> 16), static_cast<u8>(this_size >> 8),
        static_cast<u8>(this_size), static_cast<u8>(width >> 8), static_cast<u8>(width), static_cast<u8>(height >> 8),
        static_cast<u8>(height), static_cast<u8>(buffers >> 8), static_cast<u8>(buffers), 0, 0};
}

u16 be16(const std::vector<u8>& v, usize at) { return static_cast<u16>(v[at] << 8 | v[at + 1]); }
u32 be32(const std::vector<u8>& v, usize at) { return static_cast<u32>(be16(v, at)) << 16 | be16(v, at + 2); }

// Every picture item of a stream decoded in order from access unit 0: the codec, a frame buffer, the size of a YUV
// 4:2:0 picture of `width` x `height`, one access unit with its PTS, DTS and user data, a normal picture.
void check_items(const Decoded& d, u32 codec, int width, int height)
{
    for (usize i = 0; i < d.items.size(); i++) {
        INFO("item " << i);
        const std::vector<u8>& it = d.items[i];
        CHECK(be32(it, 0x00) == codec);
        CHECK(be32(it, 0x04) != 0);
        CHECK(be32(it, 0x08) == static_cast<u32>(width * height * 3 / 2));
        CHECK(it[0x0C] == 1);
        CHECK(be32(it, 0x14) == 0x1000 + 3003 * i); // PTS
        CHECK(be32(it, 0x24) == 0x800 + 3003 * i);  // DTS
        CHECK(be32(it, 0x30) == 0); // user data, 64 bits
        CHECK(be32(it, 0x34) == 0xABCD0000 + i);
        CHECK(be32(it, 0x40) == 0); // status
        CHECK(be32(it, 0x44) == 0); // attr: normal
    }
}

// CellVdecDivxInfo of DivX 3.11 as Sony's libvdec writes it (D-912): the picture type, the size, the frame rate code,
// the frame buffer's address (the item's start address), the size again, 0, 0x30, an error word 0; nothing else.
void check_divx311_info(const std::vector<u8>& info, const std::vector<u8>& item, u8 type, u16 width, u16 height,
    u16 frame_rate)
{
    std::vector<u8> want(info.size(), 0);
    want[0] = type;
    want[2] = static_cast<u8>(width >> 8), want[3] = static_cast<u8>(width);
    want[4] = static_cast<u8>(height >> 8), want[5] = static_cast<u8>(height);
    want[6] = static_cast<u8>(frame_rate >> 8), want[7] = static_cast<u8>(frame_rate);
    std::copy(item.begin() + 4, item.begin() + 8, want.begin() + 8);
    want[0xC] = want[2], want[0xD] = want[3], want[0xE] = want[4], want[0xF] = want[5];
    want[0x11] = 0x30;
    CHECK(std::equal(want.begin(), want.begin() + 0x18, info.begin()));
}

// CellVdecDivxInfo of DivX (D-912): RPCS3's fields (type, size, the VOL's aspect ratio code and pixel sizes, no colour
// description: 0s, a frame, the frame rate code), the frame buffer's address, the size again, 0, 0x30, 0.
void check_divx_info(const std::vector<u8>& info, const std::vector<u8>& item, u8 type, u16 width, u16 height, u8 par,
    u8 par_width, u8 par_height, u16 frame_rate)
{
    std::vector<u8> want(0x20, 0);
    want[0] = type;
    want[2] = static_cast<u8>(width >> 8), want[3] = static_cast<u8>(width);
    want[4] = static_cast<u8>(height >> 8), want[5] = static_cast<u8>(height);
    want[6] = par, want[7] = par_width, want[8] = par_height;
    want[0xE] = static_cast<u8>(frame_rate >> 8), want[0xF] = static_cast<u8>(frame_rate);
    std::copy(item.begin() + 4, item.begin() + 8, want.begin() + 0x10);
    want[0x14] = want[2], want[0x15] = want[3], want[0x16] = want[4], want[0x17] = want[5];
    want[0x19] = 0x30;
    CHECK(std::equal(want.begin(), want.end(), info.begin()));
}

// CellVdecMpeg4Info (D-912): the VOL's aspect ratio code, pixel sizes, time increment resolution, fixed VOP rate and
// increment, width and height; the VOP type; the picture's luma (the item's start address), Cb and Cr addresses; the
// size as words; the frame's luma lines; 3 at 0x30.
void check_mpeg4_info(const std::vector<u8>& info, const std::vector<u8>& item, u8 type, u16 width, u16 height, u8 par,
    u8 par_width, u8 par_height)
{
    CHECK(info[0] == par);
    CHECK(info[1] == par_width);
    CHECK(info[2] == par_height);
    CHECK(be16(info, 4) == 25); // vop_time_increment_resolution
    CHECK(info[6] == 0);        // fixed_vop_rate
    CHECK(be16(info, 8) == 0);
    CHECK(be16(info, 0xA) == width);
    CHECK(be16(info, 0xC) == height);
    CHECK(info[0xE] == type);
    CHECK(be32(info, 0x10) == be32(item, 4));
    const u32 strips = (width + 15) / 16 + 2, lines = (height + 15) / 16 * 16 + 32;
    const u32 frame = be32(item, 4) - lines * 16 - 16 * 16;
    const u32 cb = frame + strips * 16 * lines + (lines / 2) * 16 + 8 * 16;
    CHECK(be32(info, 0x14) == cb);
    CHECK(be32(info, 0x18) == cb + 8);
    CHECK(be32(info, 0x20) == width);
    CHECK(be32(info, 0x24) == height);
    CHECK(be32(info, 0x28) == lines);
    CHECK(info[0x30] == 3);
}

// A picture's DC levels for the VC-1 writer, different for each `seed`: blocks from 83 to 172 (levels -40 to 39).
v1::DcPicture dc_levels(int width, int height, int seed)
{
    v1::DcPicture p(width, height);
    for (int by = 0; by < height / 8; by++)
        for (int bx = 0; bx < width / 8; bx++)
            p.y[static_cast<usize>(by * (width / 8) + bx)] = -40 + (bx * 11 + by * 7 + seed * 23) % 80;
    for (int my = 0; my < height / 16; my++)
        for (int mx = 0; mx < width / 16; mx++) {
            p.cb[static_cast<usize>(my * (width / 16) + mx)] = -20 + (mx * 9 + my * 5 + seed * 13) % 40;
            p.cr[static_cast<usize>(my * (width / 16) + mx)] = 30 - (mx * 7 + my * 11 + seed * 5) % 50;
        }
    return p;
}

std::vector<u8> concat(std::initializer_list<std::vector<u8>> parts)
{
    std::vector<u8> out;
    for (const std::vector<u8>& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

// The samples of an I_PCM picture of the H.264 writer as GetPicture gives them in YUV 4:2:0.
std::vector<u8> samples_of(const h::Picture& p)
{
    return concat({p.y, p.cb, p.cr});
}

// CellVdecVc1Info as Sony's libvdec writes it (D-912), for the writer's streams: the sequence header's coded size,
// flags, display extension, aspect ratio, frame rate code and colour description, the entry point's broken link and
// closed entry, the picture's type (libsvc1d's: I 1, skipped 5) and repetition, the headers its access unit carried,
// what is valid, its addresses, the size again, the frame's lines and the range mapping.
struct Vc1Info {
    u32 width = 64, height = 48;
    u8 pulldown = 0, tfcntrflag = 0, display_ext = 0;
    u16 display_width = 0, display_height = 0;
    u8 aspect_flag = 0, aspect = 0, aspect_width = 0, aspect_height = 0;
    int frame_rate = -1; // none written
    u8 color_flag = 0, prim = 0, transfer = 0, matrix = 0;
    u8 type = 1, rptfrm = 0;
    u32 units = 7;
    u8 mapy_flag = 0, mapy = 0, mapuv_flag = 0, mapuv = 0;
};

void check_vc1_info(const std::vector<u8>& info, const std::vector<u8>& item, const Vc1Info& w, u32 picture_height)
{
    CHECK(be32(info, 0x00) == w.width);
    CHECK(be32(info, 0x04) == w.height);
    CHECK(info[0x08] == w.pulldown);
    CHECK(info[0x0A] == w.tfcntrflag);
    CHECK(info[0x0E] == w.display_ext);
    CHECK(be16(info, 0x10) == (w.display_ext ? w.display_width - 1 : 0));
    CHECK(be16(info, 0x12) == (w.display_ext ? w.display_height - 1 : 0));
    CHECK(info[0x14] == w.aspect_flag);
    CHECK(info[0x15] == w.aspect);
    CHECK(info[0x16] == (w.aspect_width ? w.aspect_width - 1 : 0));
    CHECK(info[0x17] == (w.aspect_height ? w.aspect_height - 1 : 0));
    CHECK(info[0x18] == (w.frame_rate < 0 ? 0 : w.frame_rate)); // not written: the fixture's 0
    CHECK(info[0x19] == w.color_flag);
    CHECK(info[0x1A] == w.prim);
    CHECK(info[0x1B] == w.transfer);
    CHECK(info[0x1C] == w.matrix);
    CHECK(info[0x1D] == 0); // broken link
    CHECK(info[0x1E] == 1); // closed entry
    CHECK(info[0x1F] == w.type);
    CHECK(info[0x22] == w.rptfrm);
    CHECK(info[0x23] == 1); // top field first
    CHECK(be32(info, 0x2C) == w.units);
    CHECK(be32(info, 0x30) == 7);
    CHECK(be32(info, 0x148) == be32(item, 4));
    CHECK(be32(info, 0x150) == be32(info, 0x14C) + 8);
    CHECK(be32(info, 0x158) == w.width);
    CHECK(be32(info, 0x15C) == w.height);
    CHECK(be32(info, 0x160) == (picture_height + 31) / 32 * 32 + 128);
    CHECK(info[0x168] == 3);
    CHECK(info[0x169] == w.mapy_flag);
    CHECK(info[0x16A] == w.mapuv_flag);
    CHECK(info[0x16B] == w.mapy);
    CHECK(info[0x16C] == w.mapuv);
}

} // namespace

TEST_CASE("cellVdec: DivX 3.11 on FFmpeg, its frame size from the codec information: pictures, items, information "
          "(D-588)", "[hle][vdec]")
{
    if (!avcodec_find_encoder(AV_CODEC_ID_MSMPEG4V3))
        SKIP("this FFmpeg has no msmpeg4v3 encoder (the tests' build: build-ffmpeg.sh)");
    constexpr int kW = 96, kH = 64, kFrames = 6;
    const std::vector<std::vector<u8>> packets = encode(AV_CODEC_ID_MSMPEG4V3, kW, kH, kFrames, 3);
    REQUIRE(packets.size() == kFrames);
    const std::vector<Yuv> want = ffmpeg_decode(AV_CODEC_ID_MSMPEG4V3, packets, kW, kH);
    REQUIRE(want.size() == kFrames);
    for (int i = 0; i < kFrames; i++) CHECK(psnr(want[static_cast<usize>(i)], moving(kW, kH, i)) > 35.0);

    InterpretMisses interpret;
    for (const u32 frame_rate : {0u, 0x82u}) { // the stream's (none), then SetFrameRate's 25
        INFO("SetFrameRate " << frame_rate);
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        Stream s;
        s.codec = CODEC_DIVX3_11;
        s.level = 15;
        s.spec = divx_info2(12, kW, kH, 0);
        s.aus = packets;
        s.frame_rate = frame_rate;
        const Decoded d = decode_stream(f.base(), hle_calls(f, t), s);
        INFO(all_lines(d.lines));
        REQUIRE(d.open == 0);
        CHECK(d.audone == kFrames);
        CHECK(d.pictures_after_aus == kFrames - 1); // one access unit late, as Sony's for H.264
        CHECK(d.picout == kFrames);
        CHECK(d.seqdone == 1);
        REQUIRE(d.pictures.size() == kFrames);
        check_items(d, CODEC_DIVX3_11, kW, kH);
        for (int i = 0; i < kFrames; i++) {
            INFO("picture " << i);
            CHECK(d.pictures[static_cast<usize>(i)] == want[static_cast<usize>(i)].data);
            // The frame rate code: 0, SetFrameRate's before StartSeq forgotten by it, as Sony's libdivx311dec does.
            check_divx311_info(d.infos[static_cast<usize>(i)], d.items[static_cast<usize>(i)], i % 3 == 0 ? 0 : 1, kW,
                kH, 0);
        }
    }
}

TEST_CASE("cellVdec: DivX 3.11's codec information checked as RPCS3 checks it; without it 720x576 (D-588)",
    "[hle][vdec]")
{
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    const Call call = hle_calls(f, t);
    const auto query = [&](u32 level, const std::vector<u8>& spec) {
        std::memset(b + kArgs, 0, 0x100);
        ps_write32(b, kType, CODEC_DIVX3_11);
        ps_write32(b, kType + 4, level);
        if (!spec.empty()) {
            std::memcpy(b + kSpec, spec.data(), spec.size());
            ps_write32(b, kType + 8, kSpec);
        }
        return call("cellVdecQueryAttrEx", 0xC982A84A, {kType, kAttr});
    };
    CHECK(query(15, {}) == 0);
    CHECK(ps_read32(b, kAttr) == 0x1E3578); // Sony's
    CHECK(ps_read32(b, kAttr + 12) == 0x103);
    CHECK(query(14, {}) == CELL_VDEC_ERROR_ARG);
    CHECK(query(15, divx_info2(12, 1920, 1088, 2)) == 0);
    CHECK(query(15, divx_info2(8, 96, 64, 0)) == CELL_VDEC_ERROR_ARG); // CellVdecDivxSpecificInfo: not DivX 3.11's
    CHECK(query(15, divx_info2(12, 1921, 64, 0)) == CELL_VDEC_ERROR_ARG);
    CHECK(query(15, divx_info2(12, 96, 1089, 0)) == CELL_VDEC_ERROR_ARG);
    CHECK(query(15, divx_info2(12, 96, 64, 1)) == CELL_VDEC_ERROR_ARG);

    // No frame size given: RPCS3's default, 720x576, which a stream of that size decodes with.
    if (!avcodec_find_encoder(AV_CODEC_ID_MSMPEG4V3))
        SKIP("this FFmpeg has no msmpeg4v3 encoder (the tests' build: build-ffmpeg.sh)");
    const std::vector<std::vector<u8>> packets = encode(AV_CODEC_ID_MSMPEG4V3, 720, 576, 1, 3);
    const std::vector<Yuv> want = ffmpeg_decode(AV_CODEC_ID_MSMPEG4V3, packets, 720, 576);
    REQUIRE(want.size() == 1);
    Stream s;
    s.codec = CODEC_DIVX3_11;
    s.level = 15;
    s.aus = packets;
    const Decoded d = decode_stream(b, call, s);
    INFO(all_lines(d.lines));
    REQUIRE(d.pictures.size() == 1);
    CHECK(be32(d.items[0], 0x08) == 720 * 576 * 3 / 2);
    CHECK(be16(d.infos[0], 2) == 720);
    CHECK(be16(d.infos[0], 4) == 576);
    CHECK(d.pictures[0] == want[0].data);
    // Sizes as Sony's (D-912): with the information, the frames sized for it.
    CHECK(query(15, divx_info2(12, 96, 64, 0)) == 0);
    CHECK(ps_read32(b, kAttr) == 0x9D778);
    CHECK(query(15, divx_info2(12, 1920, 1088, 2)) == 0);
    CHECK(ps_read32(b, kAttr) == 0x6D5B78);
}

TEST_CASE("cellVdec: CellVdecDivxInfo laid out as RPCS3's for DivX and MPEG-4: type, size, aspect ratio, frame rate "
          "(D-588)", "[hle][vdec]")
{
    if (!avcodec_find_encoder(AV_CODEC_ID_MPEG4))
        SKIP("this FFmpeg has no mpeg4 encoder (the tests' build: build-ffmpeg.sh)");
    constexpr int kW = 96, kH = 64, kFrames = 4;
    InterpretMisses interpret;
    // Pixel aspect ratios in the VOL: 12:11 (code 2) and 8:9 (extended, 15).
    for (const AVRational sar : {AVRational{12, 11}, AVRational{8, 9}}) {
        const std::vector<std::vector<u8>> packets =
            encode(AV_CODEC_ID_MPEG4, kW, kH, kFrames, 2, [&](AVCodecContext* av) { av->sample_aspect_ratio = sar; });
        REQUIRE(packets.size() == kFrames);
        const std::vector<Yuv> want = ffmpeg_decode(AV_CODEC_ID_MPEG4, packets);
        REQUIRE(want.size() == kFrames);
        for (const u32 codec : {CODEC_DIVX, CODEC_MPEG4}) {
            INFO("codec " << codec << " aspect " << sar.num << ":" << sar.den);
            BigFixture f;
            GuestThread t(f, 0, "ppu");
            Stream s;
            s.codec = codec;
            s.level = codec == CODEC_DIVX ? 10 : 0; // QMOBILE, SP_L1: 176x144
            s.aus = packets;
            const Decoded d = decode_stream(f.base(), hle_calls(f, t), s);
            INFO(all_lines(d.lines));
            REQUIRE(d.open == 0);
            CHECK(d.audone == kFrames);
            // DivX's pictures one access unit late, MPEG-4's at once (Sony's libdivxdec and libsmvd4, D-912).
            CHECK(d.pictures_after_aus == (codec == CODEC_DIVX ? kFrames - 1 : kFrames));
            REQUIRE(d.pictures.size() == kFrames);
            check_items(d, codec, kW, kH);
            const u8 par = sar.num == 12 ? 2 : 15, par_width = sar.num == 12 ? 0 : 8, par_height = sar.num == 12 ? 0 : 9;
            for (int i = 0; i < kFrames; i++) {
                INFO("picture " << i);
                const usize k = static_cast<usize>(i);
                CHECK(d.pictures[k] == want[k].data);
                CHECK(psnr(want[k], moving(kW, kH, i)) > 35.0);
                const u8 type = i % 2 == 0 ? 0 : 1; // I, P
                if (codec == CODEC_DIVX) check_divx_info(d.infos[k], d.items[k], type, kW, kH, par, par_width, par_height, 0);
                else check_mpeg4_info(d.infos[k], d.items[k], type, kW, kH, par, par_width, par_height);
            }
        }
    }
}

TEST_CASE("cellVdec: VC-1's advanced profile on FFmpeg: a stream written here, its pictures, items and information "
          "(D-589)", "[hle][vdec]")
{
    if (!avcodec_find_decoder(AV_CODEC_ID_VC1)) SKIP(kNoVc1);
    // A sequence header with its display extension (an extended pixel aspect ratio, 25 frames a second, a colour
    // description), an entry point, then I, skipped P, I, skipped P (each I with the headers again, as streams repeat
    // them).
    v1::Sequence seq;
    seq.display_width = 64;
    seq.display_height = 48;
    seq.aspect_ratio = 15;
    seq.aspect_width = 4;
    seq.aspect_height = 3;
    seq.framerate_nr = 2; // 25
    seq.framerate_dr = 1; // 1000
    seq.color_prim = 1;
    seq.transfer_char = 1;
    seq.matrix_coef = 6;
    const v1::Entry entry;
    const v1::DcPicture a = dc_levels(64, 48, 0), b = dc_levels(64, 48, 1);
    const std::vector<u8> headers = concat({v1::sequence_header(seq), v1::entry_point(entry)});
    const std::vector<std::vector<u8>> aus = {concat({headers, v1::intra_frame(a, seq)}), v1::skipped_frame(seq),
        concat({headers, v1::intra_frame(b, seq)}), v1::skipped_frame(seq)};
    const std::vector<std::vector<u8>> want = {v1::samples(a), v1::samples(a), v1::samples(b), v1::samples(b)};
    CHECK(v1::block_value(0) == 128);
    CHECK(v1::block_value(8) == 137);
    CHECK(v1::block_value(-40) == 83);

    // FFmpeg's own decoding of the stream (its configuration the headers) gives the pictures the writer meant.
    const std::vector<Yuv> ffmpeg = ffmpeg_decode(AV_CODEC_ID_VC1, aus, 0, 0, headers);
    REQUIRE(ffmpeg.size() == want.size());
    for (usize i = 0; i < want.size(); i++) CHECK(ffmpeg[i].data == want[i]);

    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    Stream s;
    s.codec = CODEC_VC1;
    s.level = 6; // CELL_VDEC_VC1_AP_L1
    s.aus = aus;
    const Decoded d = decode_stream(f.base(), hle_calls(f, t), s);
    INFO(all_lines(d.lines));
    REQUIRE(d.open == 0);
    CHECK(d.audone == 4);
    CHECK(d.pictures_after_aus == 3); // one access unit late
    CHECK(d.picout == 4);
    CHECK(d.seqdone == 1);
    REQUIRE(d.pictures.size() == want.size());
    check_items(d, CODEC_VC1, 64, 48);
    for (usize i = 0; i < want.size(); i++) {
        INFO("picture " << i);
        CHECK(d.pictures[i] == want[i]);
        Vc1Info w;
        w.display_ext = 1;
        w.display_width = 64;
        w.display_height = 48;
        w.aspect_flag = 1;
        w.aspect = 15;
        w.aspect_width = 4;
        w.aspect_height = 3;
        w.frame_rate = 3; // 25 a second
        w.color_flag = 1;
        w.prim = 1;
        w.transfer = 1;
        w.matrix = 6;
        w.type = i % 2 ? 5 : 1;   // skipped, I
        w.units = i % 2 ? 4 : 7; // a frame alone; the headers and a frame
        check_vc1_info(d.infos[i], d.items[i], w, 48);
    }
}

TEST_CASE("cellVdec: VC-1's range mapping left to the game in the information, as Sony's; pulldown (D-589, D-912)",
    "[hle][vdec]")
{
    if (!avcodec_find_decoder(AV_CODEC_ID_VC1)) SKIP(kNoVc1);
    v1::Sequence seq;
    seq.pulldown = true;
    seq.tfcntrflag = true;
    v1::Entry entry;
    entry.range_mapy = 7;  // luma scaled by 16 / 8 about 128
    entry.range_mapuv = 1; // chroma by 10 / 8
    const v1::DcPicture a = dc_levels(64, 48, 2);
    const std::vector<u8> headers = concat({v1::sequence_header(seq), v1::entry_point(entry)});
    const std::vector<std::vector<u8>> aus = {concat({headers, v1::intra_frame(a, seq, 2)})};
    // FFmpeg leaves the mapping out, and so does Sony's libsvc1d: the picture is the coded one.
    const std::vector<Yuv> ffmpeg = ffmpeg_decode(AV_CODEC_ID_VC1, aus, 0, 0, headers);
    REQUIRE(ffmpeg.size() == 1);
    CHECK(ffmpeg[0].data == v1::samples(a));
    const std::vector<u8> want = v1::samples(a);

    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    Stream s;
    s.codec = CODEC_VC1;
    s.level = 6; // CELL_VDEC_VC1_AP_L1 (AP_L4 asks for more memory than the fixture has)
    s.aus = aus;
    const Decoded d = decode_stream(f.base(), hle_calls(f, t), s);
    INFO(all_lines(d.lines));
    REQUIRE(d.pictures.size() == 1);
    CHECK(d.pictures[0] == want);
    Vc1Info w;
    w.pulldown = 1;
    w.tfcntrflag = 1;
    w.rptfrm = 2;
    w.mapy_flag = 1;
    w.mapy = 7;
    w.mapuv_flag = 1;
    w.mapuv = 1;
    check_vc1_info(d.infos[0], d.items[0], w, 48);
}

TEST_CASE("cellVdec: VC-1 before its sequence header, a new sequence header, the simple and main profiles opened, "
          "the decode modes (D-589, D-912)", "[hle][vdec]")
{
    if (!avcodec_find_decoder(AV_CODEC_ID_VC1)) SKIP(kNoVc1);
    v1::Sequence seq;
    v1::Sequence small;
    small.width = small.height = 32;
    const v1::Entry entry;
    const v1::DcPicture a = dc_levels(64, 48, 3), c = dc_levels(32, 32, 4);
    const std::vector<u8> headers = concat({v1::sequence_header(seq), v1::entry_point(entry)});
    InterpretMisses interpret;

    SECTION("access units before the first sequence header and entry point are skipped")
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        Stream s;
        s.codec = CODEC_VC1;
        s.level = 5;
        s.aus = {v1::skipped_frame(seq), concat({headers, v1::intra_frame(a, seq)}), v1::skipped_frame(seq)};
        const Decoded d = decode_stream(f.base(), hle_calls(f, t), s);
        INFO(all_lines(d.lines));
        CHECK(d.audone == 3);
        REQUIRE(d.pictures.size() == 2);
        for (usize i = 0; i < 2; i++) {
            INFO("picture " << i);
            CHECK(d.pictures[i] == v1::samples(a));
            CHECK(be32(d.items[i], 0x14) == 0x1000 + 3003 * (i + 1)); // the PTS of access units 1 and 2
        }
    }
    SECTION("a different sequence header opens FFmpeg's decoder again, after the pictures it holds")
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        Stream s;
        s.codec = CODEC_VC1;
        s.level = 5;
        s.aus = {concat({headers, v1::intra_frame(a, seq)}),
            concat({v1::sequence_header(small), v1::entry_point(entry), v1::intra_frame(c, small)})};
        const Decoded d = decode_stream(f.base(), hle_calls(f, t), s);
        INFO(all_lines(d.lines));
        CHECK(d.audone == 2);
        REQUIRE(d.pictures.size() == 2);
        // Announced after the 32x32 sequence header was read, the 64x48 picture is given at its size, cut (Sony's).
        const std::vector<u8> full = v1::samples(a);
        std::vector<u8> cut;
        for (int r = 0; r < 32; r++) cut.insert(cut.end(), full.begin() + r * 64, full.begin() + r * 64 + 32);
        for (int r = 0; r < 16; r++)
            cut.insert(cut.end(), full.begin() + 64 * 48 + r * 32, full.begin() + 64 * 48 + r * 32 + 16);
        for (int r = 0; r < 16; r++)
            cut.insert(cut.end(), full.begin() + 64 * 48 * 5 / 4 + r * 32, full.begin() + 64 * 48 * 5 / 4 + r * 32 + 16);
        CHECK(d.pictures[0] == cut);
        CHECK(be32(d.items[0], 0x08) == 32 * 32 * 3 / 2);
        CHECK(d.pictures[1] == v1::samples(c));
        CHECK(be32(d.items[1], 0x08) == 32 * 32 * 3 / 2);
        CHECK(be32(d.infos[1], 0x00) == 32); // the new sequence header's
        CHECK(be32(d.infos[0], 0x160) == 64 + 128); // the 64x48 picture's frame
    }
    SECTION("the simple and main profiles' levels open as Sony's; a level past the advanced profile's is ARG")
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        for (u32 level = 0; level < 4; level++) { // level 4 asks for more than the fixture has
            INFO("level " << level);
            Stream s;
            s.codec = CODEC_VC1;
            s.level = level;
            const Decoded d = decode_stream(f.base(), hle_calls(f, t), s);
            CHECK(d.open == 0);
        }
        u8* b = f.base();
        ps_write32(b, kType, CODEC_VC1);
        ps_write32(b, kType + 4, 10);
        ps_write32(b, kType + 8, 0);
        CHECK(hle_calls(f, t)("cellVdecQueryAttrEx", 0xC982A84A, {kType, kAttr}) == CELL_VDEC_ERROR_ARG);
    }
    SECTION("B and PB skipping together: taken, then a fatal error (ERROR) and nothing more; PB skipping alone ARG")
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        u8* b = f.base();
        const Call call = hle_calls(f, t);
        write_callback(b);
        std::memset(b + kArgs, 0, 0x200);
        ps_write32(b, kType, CODEC_VC1);
        ps_write32(b, kType + 4, 5);
        REQUIRE(call("cellVdecQueryAttrEx", 0xC982A84A, {kType, kAttr}) == 0);
        const u32 res[6] = {kDecoderMem, ps_read32(b, kAttr), 1000, 0x4000, 200, 1};
        for (u32 i = 0; i < 6; i++) ps_write32(b, kRes + 4 * i, res[i]);
        ps_write32(b, kCb, kOpd);
        ps_write32(b, kCb + 4, kLog);
        REQUIRE(call("cellVdecOpenEx", 0x0053E2D8, {kType, kRes, kCb, kHandle}) == 0);
        const u32 handle = ps_read32(b, kHandle);
        REQUIRE(call("cellVdecStartSeq", 0xC757C2AA, {handle}) == 0);
        const std::vector<u8> au = concat({headers, v1::intra_frame(a, seq)});
        std::memcpy(b + kStream, au.data(), au.size());
        ps_write32(b, kAu + 0, kStream);
        ps_write32(b, kAu + 4, static_cast<u32>(au.size()));
        CHECK(call("cellVdecDecodeAu", 0x2BF4DDD2, {handle, 2, kAu}) == CELL_VDEC_ERROR_ARG);
        CHECK(call("cellVdecDecodeAu", 0x2BF4DDD2, {handle, 3, kAu}) == 0);
        REQUIRE(wait_until([&] { return count_of(b, 3) >= 1; }));
        CHECK(ps_read32(b, kLog + 20) == CELL_VDEC_ERROR_FATAL); // the ERROR's data
        CHECK(call("cellVdecEndSeq", 0x824433F0, {handle}) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(log_count(b) == 1); // no AUDONE, no SEQDONE
        CHECK(call("cellVdecClose", 0x16698E83, {handle}) == 0);
    }
}

TEST_CASE("cellVdec: MVC through DecodeAu refused as Sony's (FATAL); through DecodeAuEx2 its base view decoded as the "
          "same stream without its second view would be as H.264 (D-590, D-912)",
    "[hle][vdec]")
{
    // Two views in each access unit (a subset SPS, a prefix NAL unit, the second view's coded slice extension): the
    // base view's pictures, items and information are those of the H.264 stream without them, and the second view's
    // samples (another picture) show nowhere.
    constexpr int kW = 64, kH = 64, kAus = 4;
    std::vector<std::vector<u8>> mvc, avc;
    for (int i = 0; i < kAus; i++) {
        mvc.push_back(h::mvc_access_unit(h::pattern(kW, kH, i), h::pattern(kW, kH, 10 + i), static_cast<u32>(i & 1)));
        avc.push_back(h::access_unit(h::pattern(kW, kH, i), static_cast<u32>(i & 1)));
    }
    InterpretMisses interpret;
    const auto run = [&](u32 codec, const std::vector<std::vector<u8>>& aus, bool ex2) {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        Stream s;
        s.codec = codec;
        s.level = 21;
        s.spec = {0, 0, 0, 12, 0, kW, 0, kH, 0, 0, 0, 0}; // 64x64: the memory a level 2.1 decoder asks for fits
        s.aus = aus;
        s.ex2 = ex2;
        return decode_stream(f.base(), hle_calls(f, t), s);
    };
    const Decoded as_avc = run(CODEC_AVC, avc, false);
    REQUIRE(as_avc.pictures.size() == kAus);
    for (const u32 codec : {CODEC_MVC, CODEC_MVC2}) {
        INFO("codec " << codec);
        const Decoded refused = run(codec, mvc, false);
        CHECK(refused.audone == 0);
        CHECK(refused.pictures.empty());
        CHECK(std::count(refused.lines.begin(), refused.lines.end(), "cellVdecDecodeAu -> 0x80610180") == kAus);
        const Decoded d = run(codec, mvc, true);
        INFO(all_lines(d.lines));
        REQUIRE(d.open == 0);
        CHECK(d.audone == kAus);
        CHECK(d.pictures_after_aus == kAus - 1); // one access unit late, as H.264's
        CHECK(d.picout == kAus);
        CHECK(d.seqdone == 1);
        REQUIRE(d.pictures.size() == kAus);
        for (usize i = 0; i < kAus; i++) {
            INFO("picture " << i);
            CHECK(d.pictures[i] == samples_of(h::pattern(kW, kH, static_cast<int>(i)))); // the base view's, exactly
            CHECK(d.pictures[i] == as_avc.pictures[i]);
            // CellVdecAvcInfo from the base view's SPS, PPS and NAL units, in MVC's 0x214 bytes.
            CHECK(std::equal(d.infos[i].begin(), d.infos[i].begin() + 0x214, as_avc.infos[i].begin()));
            CHECK(be32(d.items[i], 0) == codec);
            // The rest of the item but its addresses (the frame buffers come after a ring of MVC's blocks).
            CHECK(std::equal(d.items[i].begin() + 8, d.items[i].begin() + 0x48, as_avc.items[i].begin() + 8));
        }
    }
}

TEST_CASE("cellVdec: MVC with its views in access units apart: the second view's give no picture (D-590)",
    "[hle][vdec]")
{
    constexpr int kW = 64, kH = 64;
    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    Stream s;
    s.codec = CODEC_MVC;
    s.level = 21;
    s.spec = {0, 0, 0, 12, 0, kW, 0, kH, 0, 0, 0, 0};
    s.ex2 = true; // through DecodeAu Sony's libvdec refuses MVC (D-912)
    for (int i = 0; i < 2; i++) {
        s.aus.push_back(h::access_unit(h::pattern(kW, kH, i), static_cast<u32>(i)));
        s.aus.push_back(h::second_view_only(h::pattern(kW, kH, 10 + i), static_cast<u32>(i)));
    }
    const Decoded d = decode_stream(f.base(), hle_calls(f, t), s);
    INFO(all_lines(d.lines));
    CHECK(d.audone == 4); // each access unit done, the second view's with nothing decoded
    REQUIRE(d.pictures.size() == 2);
    for (usize i = 0; i < 2; i++) {
        INFO("picture " << i);
        CHECK(d.pictures[i] == samples_of(h::pattern(kW, kH, static_cast<int>(i))));
        CHECK(be32(d.items[i], 0x14) == 0x1000 + 3003 * 2 * i); // the PTS of access units 0 and 2
    }
}
#endif
