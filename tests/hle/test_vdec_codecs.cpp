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

// The CellVdecDivxInfo fields (D-588), the rest of the block zero.
struct DivxInfo {
    u8 type = 0;
    u16 width = 0, height = 0;
    u8 par = 1, par_width = 0, par_height = 0, colour = 0, primaries = 1, transfer = 1, matrix = 1, structure = 0;
    u16 frame_rate = 0;
};

void check_divx_info(const std::vector<u8>& info, const DivxInfo& want)
{
    CHECK(info[0] == want.type);
    CHECK(info[1] == 0);
    CHECK(be16(info, 2) == want.width);
    CHECK(be16(info, 4) == want.height);
    CHECK(info[6] == want.par);
    CHECK(info[7] == want.par_width);
    CHECK(info[8] == want.par_height);
    CHECK(info[9] == want.colour);
    CHECK(info[10] == want.primaries);
    CHECK(info[11] == want.transfer);
    CHECK(info[12] == want.matrix);
    CHECK(info[13] == want.structure);
    CHECK(be16(info, 14) == want.frame_rate);
    bool rest_zero = true;
    for (usize k = 16; k < info.size(); k++) rest_zero &= info[k] == 0;
    CHECK(rest_zero);
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

// VC-1's range mapping of YUV 4:2:0 planes (SMPTE 421M's RANGE_MAPY and RANGE_MAPUV): each sample scaled about 128 by
// (R + 9) / 8.
std::vector<u8> range_mapped(std::vector<u8> yuv, int width, int height, int ry, int ruv)
{
    for (usize k = 0; k < yuv.size(); k++) {
        const int r = k < static_cast<usize>(width * height) ? ry : ruv;
        yuv[k] = static_cast<u8>(std::clamp((((yuv[k] - 128) * (r + 9) + 4) >> 3) + 128, 0, 255));
    }
    return yuv;
}

// The samples of an I_PCM picture of the H.264 writer as GetPicture gives them in YUV 4:2:0.
std::vector<u8> samples_of(const h::Picture& p)
{
    return concat({p.y, p.cb, p.cr});
}

// CellVdecVc1Info's fields (pspocket's layout, D-589), the rest of the block zero.
struct Vc1Info {
    u16 width = 0, height = 0;
    u8 type0 = 0, type1 = 0, fcm = 0, tff = 1, rff = 0, rptfrm = 0, level = 0, interlace = 0, pulldown = 0,
       display_ext = 0;
    u16 max_width = 0, max_height = 0, display_width = 0, display_height = 0;
    u8 aspect = 0, framerate_flag = 0;
    u16 aspect_width = 0, aspect_height = 0;
    u8 framerate_ind = 0, framerate_nr = 0, framerate_dr = 0, color_flag = 0;
    u16 framerate_exp = 0;
    u8 prim = 0, transfer = 0, matrix = 0, broken_link = 0, closed_entry = 1, mapy_flag = 0, mapy = 0, mapuv_flag = 0,
       mapuv = 0;
};

void check_vc1_info(const std::vector<u8>& info, const Vc1Info& w)
{
    CHECK(be16(info, 0x00) == w.width);
    CHECK(be16(info, 0x02) == w.height);
    CHECK(info[0x04] == w.type0);
    CHECK(info[0x05] == w.type1);
    CHECK(info[0x06] == w.fcm);
    CHECK(info[0x07] == w.tff);
    CHECK(info[0x08] == w.rff);
    CHECK(info[0x09] == w.rptfrm);
    CHECK(info[0x0A] == w.level);
    CHECK(info[0x0B] == w.interlace);
    CHECK(info[0x0C] == w.pulldown);
    CHECK(info[0x0D] == w.display_ext);
    CHECK(be16(info, 0x0E) == w.max_width);
    CHECK(be16(info, 0x10) == w.max_height);
    CHECK(be16(info, 0x12) == w.display_width);
    CHECK(be16(info, 0x14) == w.display_height);
    CHECK(info[0x16] == w.aspect);
    CHECK(info[0x17] == w.framerate_flag);
    CHECK(be16(info, 0x18) == w.aspect_width);
    CHECK(be16(info, 0x1A) == w.aspect_height);
    CHECK(info[0x1C] == w.framerate_ind);
    CHECK(info[0x1D] == w.framerate_nr);
    CHECK(info[0x1E] == w.framerate_dr);
    CHECK(info[0x1F] == w.color_flag);
    CHECK(be16(info, 0x20) == w.framerate_exp);
    CHECK(info[0x22] == w.prim);
    CHECK(info[0x23] == w.transfer);
    CHECK(info[0x24] == w.matrix);
    CHECK(info[0x25] == w.broken_link);
    CHECK(info[0x26] == w.closed_entry);
    CHECK(info[0x27] == w.mapy_flag);
    CHECK(info[0x28] == w.mapy);
    CHECK(info[0x29] == w.mapuv_flag);
    CHECK(info[0x2A] == w.mapuv);
    bool rest_zero = true;
    for (usize k = 0x2B; k < info.size(); k++) rest_zero &= info[k] == 0;
    CHECK(rest_zero);
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
            DivxInfo w;
            w.type = i % 3 == 0 ? 0 : 1; // I, P, P
            w.width = kW;
            w.height = kH;
            w.frame_rate = frame_rate ? 3 : 0; // CELL_VDEC_DIVX_FRC_25, else UNDEFINED
            check_divx_info(d.infos[static_cast<usize>(i)], w);
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
            CHECK(d.pictures_after_aus == kFrames - 1);
            REQUIRE(d.pictures.size() == kFrames);
            check_items(d, codec, kW, kH);
            for (int i = 0; i < kFrames; i++) {
                INFO("picture " << i);
                CHECK(d.pictures[static_cast<usize>(i)] == want[static_cast<usize>(i)].data);
                CHECK(psnr(want[static_cast<usize>(i)], moving(kW, kH, i)) > 35.0);
                DivxInfo w;
                w.type = i % 2 == 0 ? 0 : 1; // I, P
                w.width = kW;
                w.height = kH;
                w.par = sar.num == 12 ? 2 : 15;
                w.par_width = sar.num == 12 ? 0 : 8;
                w.par_height = sar.num == 12 ? 0 : 9;
                w.frame_rate = 3; // 25 a second, from the VOL's time increment resolution
                check_divx_info(d.infos[static_cast<usize>(i)], w);
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
        w.width = w.max_width = w.display_width = 64;
        w.height = w.max_height = w.display_height = 48;
        w.type0 = w.type1 = i % 2 ? 4 : 0; // skipped, I
        w.level = 1;
        w.display_ext = 1;
        w.aspect = 15;
        w.aspect_width = 4;
        w.aspect_height = 3;
        w.framerate_flag = 1;
        w.framerate_nr = 2;
        w.framerate_dr = 1;
        w.color_flag = 1;
        w.prim = 1;
        w.transfer = 1;
        w.matrix = 6;
        check_vc1_info(d.infos[i], w);
    }
}

TEST_CASE("cellVdec: VC-1's range mapping applied to the pictures, pulldown in the information (D-589)", "[hle][vdec]")
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
    // FFmpeg leaves the mapping out: its picture is the coded one.
    const std::vector<Yuv> ffmpeg = ffmpeg_decode(AV_CODEC_ID_VC1, aus, 0, 0, headers);
    REQUIRE(ffmpeg.size() == 1);
    CHECK(ffmpeg[0].data == v1::samples(a));
    const std::vector<u8> want = range_mapped(v1::samples(a), 64, 48, 7, 1);
    CHECK(range_mapped({137, 128, 100}, 1, 1, 7, 7) == std::vector<u8>{146, 128, 72}); // by hand

    InterpretMisses interpret;
    BigFixture f;
    GuestThread t(f, 0, "ppu");
    Stream s;
    s.codec = CODEC_VC1;
    s.level = 9; // CELL_VDEC_VC1_AP_L4
    s.aus = aus;
    const Decoded d = decode_stream(f.base(), hle_calls(f, t), s);
    INFO(all_lines(d.lines));
    REQUIRE(d.pictures.size() == 1);
    CHECK(d.pictures[0] == want);
    Vc1Info w;
    w.width = w.max_width = 64;
    w.height = w.max_height = 48;
    w.level = 1;
    w.pulldown = 1;
    w.rptfrm = 2;
    w.mapy_flag = 1;
    w.mapy = 7;
    w.mapuv_flag = 1;
    w.mapuv = 1;
    check_vc1_info(d.infos[0], w);
}

TEST_CASE("cellVdec: VC-1 before its sequence header, a new sequence header, the simple and main profiles refused, "
          "the decode modes (D-589)", "[hle][vdec]")
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
        CHECK(d.pictures[0] == v1::samples(a));
        CHECK(d.pictures[1] == v1::samples(c));
        CHECK(be32(d.items[1], 0x08) == 32 * 32 * 3 / 2);
        CHECK(be16(d.infos[1], 0x00) == 32);
        CHECK(be16(d.infos[1], 0x0E) == 32); // the new sequence header's
    }
    SECTION("the simple and main profiles' levels open FATAL; a level past the advanced profile's is ARG")
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        for (u32 level = 0; level < 5; level++) {
            INFO("level " << level);
            Stream s;
            s.codec = CODEC_VC1;
            s.level = level;
            const Decoded d = decode_stream(f.base(), hle_calls(f, t), s);
            CHECK(d.lines.size() == 2); // QueryAttrEx answered, OpenEx refused
            CHECK(d.open == CELL_VDEC_ERROR_FATAL);
        }
        u8* b = f.base();
        ps_write32(b, kType, CODEC_VC1);
        ps_write32(b, kType + 4, 10);
        ps_write32(b, kType + 8, 0);
        CHECK(hle_calls(f, t)("cellVdecQueryAttrEx", 0xC982A84A, {kType, kAttr}) == CELL_VDEC_ERROR_ARG);
    }
    SECTION("B and PB skipping together is VC-1's (the picture marked skipped), PB skipping alone is not")
    {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        Stream s;
        s.codec = CODEC_VC1;
        s.level = 5;
        s.aus = {concat({headers, v1::intra_frame(a, seq)})};
        s.mode = 3;
        const Decoded d = decode_stream(f.base(), hle_calls(f, t), s);
        INFO(all_lines(d.lines));
        REQUIRE(d.pictures.size() == 1);
        CHECK(d.pictures[0] == v1::samples(a));
        CHECK(be32(d.items[0], 0x44) == 1); // CELL_VDEC_PICITEM_ATTR_SKIPPED
        s.mode = 2;
        const Decoded e = decode_stream(f.base(), hle_calls(f, t), s);
        INFO(all_lines(e.lines));
        CHECK(e.audone == 0);
        CHECK(e.pictures.empty());
        CHECK(std::count(e.lines.begin(), e.lines.end(), "cellVdecDecodeAu -> 0x80610101") == 1);
    }
}

TEST_CASE("cellVdec: MVC's base view decoded as the same stream without its second view would be as H.264 (D-590)",
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
    const auto run = [&](u32 codec, const std::vector<std::vector<u8>>& aus) {
        BigFixture f;
        GuestThread t(f, 0, "ppu");
        Stream s;
        s.codec = codec;
        s.level = 21;
        s.aus = aus;
        return decode_stream(f.base(), hle_calls(f, t), s);
    };
    const Decoded as_avc = run(CODEC_AVC, avc);
    REQUIRE(as_avc.pictures.size() == kAus);
    for (const u32 codec : {CODEC_MVC, CODEC_MVC2}) {
        INFO("codec " << codec);
        const Decoded d = run(codec, mvc);
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
            CHECK(d.infos[i] == as_avc.infos[i]); // CellVdecAvcInfo from the base view's SPS, PPS and NAL units
            CHECK(be32(d.items[i], 0) == codec);
            CHECK(std::equal(d.items[i].begin() + 4, d.items[i].end(), as_avc.items[i].begin() + 4));
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
