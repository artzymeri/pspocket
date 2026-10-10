// cellVdec's codecs past H.264 (D-588): DivX 3.11 and MPEG-4 part 2 streams written by FFmpeg's own encoders (the
// tests' build of FFmpeg, D-577), each decoded through pspocket's libvdec and compared with FFmpeg's own decoding of
// the same packets: the answers, callbacks, picture items, information blocks and pixels. No game data.

#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <string>
#include <vector>

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

#if PSPOCKET_HAVE_FFMPEG
namespace {

constexpr u32 CODEC_MPEG4 = 2, CODEC_DIVX = 5, CODEC_DIVX3_11 = 9;
constexpr u32 CELL_VDEC_ERROR_ARG = 0x80610101;

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

// FFmpeg's own decoding of `packets` by decoder `id` (the frame size given for a codec whose stream has none).
std::vector<Yuv> ffmpeg_decode(AVCodecID id, const std::vector<std::vector<u8>>& packets, int width = 0, int height = 0)
{
    std::vector<Yuv> out;
    const AVCodec* codec = avcodec_find_decoder(id);
    REQUIRE(codec);
    AVCodecContext* av = avcodec_alloc_context3(codec);
    av->width = av->coded_width = width;
    av->height = av->coded_height = height;
    av->thread_count = 1;
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
#endif
