#pragma once

// H.264 on the media engine (D-254): FFmpeg's decoder with its VideoToolbox hwaccel. FFmpeg still parses the stream
// and puts the pictures in order; the hardware decodes the slices. cellVdec asks for it on every H.264 decoder it
// opens and falls back to FFmpeg's software decoding when the device or a picture fails (logged once).
// PSPOCKET_VDEC_SOFTWARE=1 in the environment keeps the software decoder (A/B runs, tests).

#include <cstdint>
#include <vector>

#if PSPOCKET_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

namespace pspocket::hle::vdec_hw {

// Asks `av` (an H.264 decoder context, not opened yet) to decode on VideoToolbox. False when this build or device has
// none, or the environment says software: `av` is left as it was.
bool attach(AVCodecContext* av);

// Whether `f` holds a hardware picture (VideoToolbox's pixel buffer).
bool is_hardware(const AVFrame* f);

// A decoded 8-bit 4:2:0 picture in three planes, from a software (YUV420P/YUVJ420P) or hardware (NV12 once
// transferred) frame. False when the frame is neither, or the transfer failed.
bool planes(const AVFrame* f, uint32_t width, uint32_t height, std::vector<uint8_t>& y, std::vector<uint8_t>& u,
    std::vector<uint8_t>& v);

} // namespace pspocket::hle::vdec_hw
#endif
