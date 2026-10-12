#pragma once

// The video codec cores' decoding on FFmpeg (D-920): what a Sony core's SPU program does with an access unit, done on
// the host. A core library of pspocket's (vdec_cores.cpp) gives each access unit to one of these and places the
// pictures that come out in its frame buffers as Sony's core lays them out (vdec_layout.hpp). The same FFmpeg decoders
// cellVdec uses (D-181, D-588, D-589).

#include <cstring>
#include <deque>
#include <vector>

#include "pspocket/core/types.hpp"

#if PSPOCKET_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}
#include "vdec_hw.hpp"
#endif

namespace pspocket::hle::vdec {

// A decoded picture: planar 4:2:0 without padding, its picture type (FFmpeg's AV_PICTURE_TYPE_*), whether FFmpeg
// concealed errors in it, and the index of the access unit it came from.
struct DecodedPicture {
    u32 width = 0, height = 0;
    int pict_type = 0;
    bool key = false, damaged = false;
    u64 au = 0;
    std::vector<u8> y, u, v;
};

class FfDecoder {
public:
    FfDecoder() = default;
    FfDecoder(const FfDecoder&) = delete;
    FfDecoder& operator=(const FfDecoder&) = delete;
    ~FfDecoder() { close(); }

#if PSPOCKET_HAVE_FFMPEG
    // Opens FFmpeg's decoder `id` (the frame size for the codecs whose stream has none, DivX 3.11's; else 0), with
    // `extradata` as its configuration (VC-1's sequence header and entry point) and `idct` its IDCT (FF_IDCT_*, 0
    // FFmpeg's choice). False when there is none or it refuses (the caller logs).
    bool open(AVCodecID id, u32 width = 0, u32 height = 0, const std::vector<u8>& extradata = {}, int idct = 0)
    {
        close();
        const AVCodec* dec = avcodec_find_decoder(id);
        if (!dec) return false;
        av_ = avcodec_alloc_context3(dec);
        av_->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
        av_->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
        av_->thread_count = 1; // each picture out as soon as FFmpeg has it
        if (idct) av_->idct_algo = idct;
        if (width && height) {
            av_->width = av_->coded_width = static_cast<int>(width);
            av_->height = av_->coded_height = static_cast<int>(height);
        }
        if (!extradata.empty()) {
            av_->extradata = static_cast<u8*>(av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            std::memcpy(av_->extradata, extradata.data(), extradata.size());
            av_->extradata_size = static_cast<int>(extradata.size());
        }
        if (avcodec_open2(av_, dec, nullptr) < 0) {
            avcodec_free_context(&av_);
            return false;
        }
        return true;
    }
#endif

    bool is_open() const
    {
#if PSPOCKET_HAVE_FFMPEG
        return av_ != nullptr;
#else
        return false;
#endif
    }

    void close()
    {
#if PSPOCKET_HAVE_FFMPEG
        avcodec_free_context(&av_);
#endif
    }

    // Decodes one access unit (copied, with FFmpeg's padding after it); `skip` the pictures FFmpeg may leave out
    // (AVDISCARD_*). The pictures that came out, in output order. Whether FFmpeg took the packet.
    bool decode(const u8* data, u32 size, u64 au, std::deque<DecodedPicture>& out, [[maybe_unused]] int skip = 0)
    {
#if PSPOCKET_HAVE_FFMPEG
        if (!av_) return false;
        std::vector<u8> copy(size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        std::memcpy(copy.data(), data, size);
        AVPacket* pkt = av_packet_alloc();
        pkt->data = copy.data();
        pkt->size = static_cast<int>(size);
        pkt->opaque = reinterpret_cast<void*>(static_cast<uintptr_t>(au));
        av_->skip_frame = static_cast<AVDiscard>(skip ? skip : AVDISCARD_DEFAULT);
        const bool taken = avcodec_send_packet(av_, pkt) >= 0;
        av_packet_free(&pkt);
        receive(out);
        return taken;
#else
        (void)data, (void)size, (void)au, (void)out;
        return false;
#endif
    }

    // The pictures FFmpeg still holds (the end of the stream), then the decoder as new.
    void drain(std::deque<DecodedPicture>& out)
    {
#if PSPOCKET_HAVE_FFMPEG
        if (!av_) return;
        avcodec_send_packet(av_, nullptr);
        receive(out);
        avcodec_flush_buffers(av_);
#else
        (void)out;
#endif
    }

    // Forgets what the decoder holds (references, pictures not out).
    void reset()
    {
#if PSPOCKET_HAVE_FFMPEG
        if (av_) avcodec_flush_buffers(av_);
#endif
    }

private:
#if PSPOCKET_HAVE_FFMPEG
    void receive(std::deque<DecodedPicture>& out)
    {
        AVFrame* f = av_frame_alloc();
        while (avcodec_receive_frame(av_, f) == 0) {
            DecodedPicture p;
            p.width = static_cast<u32>(f->width);
            p.height = static_cast<u32>(f->height);
            p.pict_type = f->pict_type;
            p.key = (f->flags & AV_FRAME_FLAG_KEY) != 0;
            p.damaged = f->decode_error_flags != 0 || (f->flags & AV_FRAME_FLAG_CORRUPT) != 0;
            p.au = static_cast<u64>(reinterpret_cast<uintptr_t>(f->opaque));
            if (vdec_hw::planes(f, p.width, p.height, p.y, p.u, p.v)) out.push_back(std::move(p));
            av_frame_unref(f);
        }
        av_frame_free(&f);
    }

    AVCodecContext* av_ = nullptr;
#endif
};

} // namespace pspocket::hle::vdec
