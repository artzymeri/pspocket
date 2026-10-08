// cellVdec's H.264 on VideoToolbox (D-254) against FFmpeg's software decoder on real H.264: a moving pattern encoded
// here by the Mac's own encoder (VideoToolbox, CABAC, P and B pictures), then decoded both ways. Nothing comes from
// a game. macOS only; skipped elsewhere.

#if PSPOCKET_HAVE_FFMPEG && defined(__APPLE__)

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <catch_amalgamated.hpp>
#include <chrono>
#include <cstring>
#include <ctime>
#include <vector>

#include "vdec_hw.hpp"

namespace {

struct Encoded {
    std::vector<std::vector<uint8_t>> access_units; // Annex B, decode order; the first carries SPS and PPS
};

void encoded_frame(void* user, void*, OSStatus status, VTEncodeInfoFlags, CMSampleBufferRef sample)
{
    auto& out = *static_cast<Encoded*>(user);
    if (status != noErr || !sample) return;
    std::vector<uint8_t> au;
    const auto start_code = [&] { au.insert(au.end(), {0, 0, 0, 1}); };
    if (out.access_units.empty()) {
        CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sample);
        size_t count = 0;
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, nullptr, nullptr, &count, nullptr);
        for (size_t i = 0; i < count; i++) {
            const uint8_t* ps = nullptr;
            size_t size = 0;
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &ps, &size, nullptr, nullptr);
            start_code();
            au.insert(au.end(), ps, ps + size);
        }
    }
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    size_t length = 0;
    char* data = nullptr;
    CMBlockBufferGetDataPointer(block, 0, nullptr, &length, &data);
    for (size_t at = 0; at + 4 <= length;) { // AVCC: big-endian 4-byte lengths
        const uint32_t n = (uint32_t(uint8_t(data[at])) << 24) | (uint32_t(uint8_t(data[at + 1])) << 16) |
            (uint32_t(uint8_t(data[at + 2])) << 8) | uint8_t(data[at + 3]);
        start_code();
        au.insert(au.end(), data + at + 4, data + at + 4 + n);
        at += 4 + n;
    }
    out.access_units.push_back(std::move(au));
}

// `frames` pictures of a moving gradient with a sliding square, width x height, encoded at a high bit rate.
Encoded encode(int width, int height, int frames)
{
    Encoded out;
    VTCompressionSessionRef session = nullptr;
    REQUIRE(VTCompressionSessionCreate(nullptr, width, height, kCMVideoCodecType_H264, nullptr, nullptr, nullptr,
                encoded_frame, &out, &session) == noErr);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanTrue);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel);
    const int bitrate = width * height * 4;
    CFNumberRef rate = CFNumberCreate(nullptr, kCFNumberIntType, &bitrate);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AverageBitRate, rate);
    CFRelease(rate);
    for (int i = 0; i < frames; i++) {
        CVPixelBufferRef pb = nullptr;
        REQUIRE(CVPixelBufferCreate(nullptr, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, nullptr,
                    &pb) == kCVReturnSuccess);
        CVPixelBufferLockBaseAddress(pb, 0);
        auto* y = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 0));
        auto* uv = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 1));
        const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
        for (int r = 0; r < height; r++)
            for (int c = 0; c < width; c++) {
                const bool square = c >= 40 + i * 6 && c < 140 + i * 6 && r >= 60 && r < 160;
                y[r * ys + c] = square ? 235 : static_cast<uint8_t>(16 + (c + r + i * 3) % 200);
            }
        for (int r = 0; r < height / 2; r++)
            for (int c = 0; c < width / 2; c++) {
                uv[r * uvs + 2 * c] = static_cast<uint8_t>(64 + (c * 2 + i) % 128);
                uv[r * uvs + 2 * c + 1] = static_cast<uint8_t>(64 + (r * 2 + i * 2) % 128);
            }
        CVPixelBufferUnlockBaseAddress(pb, 0);
        REQUIRE(VTCompressionSessionEncodeFrame(session, pb, CMTimeMake(i, 30), kCMTimeInvalid, nullptr, nullptr,
                    nullptr) == noErr);
        CVPixelBufferRelease(pb);
    }
    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    return out;
}

struct Decoded {
    std::vector<std::vector<uint8_t>> pictures; // Y, then Cb, then Cr
    bool hardware = false;
    double cpu_s = 0;
};

double thread_cpu_s()
{
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

Decoded decode(const Encoded& in, int width, int height, bool hardware)
{
    Decoded out;
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->thread_count = 1; // as cellVdec opens it
    const bool attached = hardware && pspocket::hle::vdec_hw::attach(ctx);
    REQUIRE(avcodec_open2(ctx, codec, nullptr) == 0);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const double t0 = thread_cpu_s();
    const auto drain = [&] {
        while (avcodec_receive_frame(ctx, frame) == 0) {
            out.hardware |= pspocket::hle::vdec_hw::is_hardware(frame);
            std::vector<uint8_t> y, u, v;
            REQUIRE(pspocket::hle::vdec_hw::planes(frame, static_cast<uint32_t>(width), static_cast<uint32_t>(height), y,
                u, v));
            y.insert(y.end(), u.begin(), u.end());
            y.insert(y.end(), v.begin(), v.end());
            out.pictures.push_back(std::move(y));
            av_frame_unref(frame);
        }
    };
    for (const auto& au : in.access_units) {
        pkt->data = const_cast<uint8_t*>(au.data());
        pkt->size = static_cast<int>(au.size());
        REQUIRE(avcodec_send_packet(ctx, pkt) == 0);
        drain();
    }
    REQUIRE(avcodec_send_packet(ctx, nullptr) == 0);
    drain();
    out.cpu_s = thread_cpu_s() - t0;
    out.hardware &= attached;
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    return out;
}

} // namespace

TEST_CASE("cellVdec: real H.264 decodes on VideoToolbox to the software decoder's pictures, sample for sample (D-254)",
    "[hle][vdec]")
{
    constexpr int kW = 1280, kH = 720, kFrames = 60;
    const Encoded stream = encode(kW, kH, kFrames);
    REQUIRE(stream.access_units.size() >= kFrames - 2); // the encoder may drop a picture
    const Decoded software = decode(stream, kW, kH, false);
    const Decoded hardware = decode(stream, kW, kH, true);
    REQUIRE(software.pictures.size() + 2 >= stream.access_units.size()); // an access unit may hold no picture
    if (!hardware.hardware) {
        WARN("VideoToolbox did not decode here: only the software path was checked");
        return;
    }
    REQUIRE(hardware.pictures.size() == software.pictures.size());
    size_t same = 0;
    for (size_t i = 0; i < software.pictures.size(); i++) same += hardware.pictures[i] == software.pictures[i];
    CHECK(same == software.pictures.size());
    // What it costs the decoding thread (rule 8): FFmpeg's software decoder against the media engine.
    WARN("1280x720, " << software.pictures.size() << " pictures: decoding thread CPU " << software.cpu_s * 1000 << " ms in software, "
                      << hardware.cpu_s * 1000 << " ms on VideoToolbox");
}

#endif
