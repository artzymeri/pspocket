#pragma once

// The cellVdec codec tests' streams (D-588 to D-590), for the comparison with Sony's libvdec (D-597): DivX 3.11 and
// MPEG-4 part 2 written by FFmpeg's own encoders (the tests' build of FFmpeg), and the pictures the VC-1 writer
// (vc1_writer.hpp) is given. The same functions as test_vdec_codecs.cpp's, so both tests decode the same streams (that
// file keeps its own copies while cellVdec's tests change elsewhere; it can include this one instead).

#if PSPOCKET_HAVE_FFMPEG

#include <catch_amalgamated.hpp>

#include <cstring>
#include <functional>
#include <initializer_list>
#include <vector>

#include "pspocket/core/types.hpp"
#include "vc1_writer.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

namespace pspocket::hle::test::vdec_streams {

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
inline Yuv moving(int width, int height, int i)
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

// `frames` pictures written by FFmpeg's encoder for `id` at a constant quantiser, 25 a second, an I picture every
// `gop`: one packet each.
inline std::vector<std::vector<u8>> encode(AVCodecID id, int width, int height, int frames, int gop,
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

// CellVdecDivxSpecificInfo2 (RPCS3 cellVdec.h): thisSize, maxDecodedFrameWidth, maxDecodedFrameHeight,
// numberOfDecodedFrameBuffer, big-endian.
inline std::vector<u8> divx_info2(u32 this_size, u16 width, u16 height, u16 buffers)
{
    return {static_cast<u8>(this_size >> 24), static_cast<u8>(this_size >> 16), static_cast<u8>(this_size >> 8),
        static_cast<u8>(this_size), static_cast<u8>(width >> 8), static_cast<u8>(width), static_cast<u8>(height >> 8),
        static_cast<u8>(height), static_cast<u8>(buffers >> 8), static_cast<u8>(buffers), 0, 0};
}

// A picture's DC levels for the VC-1 writer, different for each `seed`: blocks from 83 to 172 (levels -40 to 39).
inline vc1::DcPicture dc_levels(int width, int height, int seed)
{
    vc1::DcPicture p(width, height);
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

inline std::vector<u8> concat(std::initializer_list<std::vector<u8>> parts)
{
    std::vector<u8> out;
    for (const std::vector<u8>& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

} // namespace pspocket::hle::test::vdec_streams

#endif
