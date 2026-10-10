// H.264 on the media engine through FFmpeg's VideoToolbox hwaccel (D-254). See vdec_hw.hpp.

#include "vdec_hw.hpp"

#if PSPOCKET_HAVE_FFMPEG

#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include "pspocket/core/log.hpp"

namespace pspocket::hle::vdec_hw {
namespace {

// The VideoToolbox path's helpers: Apple's builds only (FFmpeg on Linux has no hwaccel, D-576).
#if defined(__APPLE__)
constexpr std::string_view kChan = "cellVdec";

// FFmpeg offers the hardware format first when the hwaccel can take the stream; otherwise the software one.
AVPixelFormat pick_format(AVCodecContext*, const AVPixelFormat* formats)
{
    const AVPixelFormat* first_software = nullptr;
    for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; f++) {
        if (*f == AV_PIX_FMT_VIDEOTOOLBOX) return *f;
        if (!first_software && !(av_pix_fmt_desc_get(*f)->flags & AV_PIX_FMT_FLAG_HWACCEL)) first_software = f;
    }
    return first_software ? *first_software : formats[0];
}

bool software_only()
{
    const char* env = std::getenv("PSPOCKET_VDEC_SOFTWARE");
    return env && env[0] && env[0] != '0';
}
#endif

} // namespace

bool attach(AVCodecContext* av)
{
#if defined(__APPLE__)
    if (software_only() || av->codec_id != AV_CODEC_ID_H264) return false;
    AVBufferRef* device = nullptr;
    if (const int r = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0); r < 0) {
        static bool logged = false;
        if (!logged) PS_LOG_WARN(kChan, "VideoToolbox is not available here ({}): H.264 decodes in software", r);
        logged = true;
        return false;
    }
    av->hw_device_ctx = device; // the context owns the reference now
    av->get_format = pick_format;
    return true;
#else
    (void)av;
    return false;
#endif
}

bool is_hardware(const AVFrame* f) { return f->format == AV_PIX_FMT_VIDEOTOOLBOX; }

bool planes(const AVFrame* f, uint32_t width, uint32_t height, std::vector<uint8_t>& y, std::vector<uint8_t>& u,
    std::vector<uint8_t>& v)
{
    const AVFrame* src = f;
    AVFrame* sw = nullptr;
    if (is_hardware(f)) {
        sw = av_frame_alloc();
        if (av_hwframe_transfer_data(sw, f, 0) < 0) {
            av_frame_free(&sw);
            return false;
        }
        src = sw;
    }
    const auto fmt = static_cast<AVPixelFormat>(src->format);
    const bool planar = fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P;
    const bool nv12 = fmt == AV_PIX_FMT_NV12;
    if (!planar && !nv12) {
        av_frame_free(&sw);
        return false;
    }
    y.resize(width * height);
    u.resize(width * height / 4);
    v.resize(width * height / 4);
    for (uint32_t r = 0; r < height; r++) std::memcpy(&y[r * width], src->data[0] + r * src->linesize[0], width);
    for (uint32_t r = 0; r < height / 2; r++) {
        if (planar) {
            std::memcpy(&u[r * width / 2], src->data[1] + r * src->linesize[1], width / 2);
            std::memcpy(&v[r * width / 2], src->data[2] + r * src->linesize[2], width / 2);
        } else {
            const uint8_t* uv = src->data[1] + r * src->linesize[1]; // Cb Cr interleaved
            for (uint32_t c = 0; c < width / 2; c++) {
                u[r * width / 2 + c] = uv[2 * c];
                v[r * width / 2 + c] = uv[2 * c + 1];
            }
        }
    }
    av_frame_free(&sw);
    return true;
}

} // namespace pspocket::hle::vdec_hw
#endif
