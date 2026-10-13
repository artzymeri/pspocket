#pragma once

// The video codec cores' decoding on FFmpeg (D-920): what a Sony core's SPU program does with an access unit, done on
// the host. A core library of pspocket's (vdec_cores.cpp) gives each access unit to one of these and places the
// pictures that come out in its frame buffers as Sony's core lays them out (vdec_layout.hpp). The same FFmpeg decoders
// cellVdec uses (D-181, D-588, D-589).

#include <cstring>
#include <deque>
#include <map>
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
    // `extradata` as its configuration (VC-1's sequence header and entry point), `idct` its IDCT (FF_IDCT_*, 0
    // FFmpeg's choice) and `flags` more of its AV_CODEC_FLAG_*s. False when there is none or it refuses (the caller
    // logs).
    bool open(AVCodecID id, u32 width = 0, u32 height = 0, const std::vector<u8>& extradata = {}, int idct = 0,
        int flags = 0)
    {
        close();
        const AVCodec* dec = avcodec_find_decoder(id);
        if (!dec) return false;
        av_ = avcodec_alloc_context3(dec);
        av_->opaque = this;
        av_->get_buffer2 = get_buffer;
        av_->flags |= AV_CODEC_FLAG_COPY_OPAQUE | flags;
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
        av_frame_free(&last_);
        forget_all_but(0, 0);
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
        if (last_) av_frame_unref(last_);
        forget_all_but(0, 0);
#endif
    }

    // FFmpeg's error concealment (FF_EC_*, its default FF_EC_GUESS_MVS | FF_EC_DEBLOCK) for what it decodes next.
    void concealment([[maybe_unused]] int ec)
    {
#if PSPOCKET_HAVE_FFMPEG
        if (av_) av_->error_concealment = ec;
#endif
    }

    // The next picture FFmpeg decodes starts as these planes (planar 4:2:0, no padding, w x h) instead of whatever its
    // buffer held: what it does not decode (macroblocks missing from a damaged access unit, with error concealment off)
    // keeps them, as a core whose SPU program leaves a frame buffer's samples it does not decode (D-920).
    void prefill(u32 w, u32 h, std::vector<u8> y, std::vector<u8> u, std::vector<u8> v)
    {
        prefill_w_ = w;
        prefill_h_ = h;
        prefill_y_ = std::move(y);
        prefill_u_ = std::move(u);
        prefill_v_ = std::move(v);
    }

    // Keeps the last picture FFmpeg gave (a reference to its buffer), so that rewrite_last() can change it: for a core
    // whose SPU program predicts a P picture from a frame buffer's contents, whatever they hold (D-920). Only for
    // decoders that give each picture as soon as it is decoded and predict the next from it (MPEG-4 part 2 with
    // AV_CODEC_FLAG_LOW_DELAY, B pictures never given): FFmpeg's reference is then that picture's buffer.
    void keep_last(bool on) { keep_last_ = on; }

    // The last picture's planes (planar 4:2:0, no padding, w x h) rewritten, so that FFmpeg predicts the next P picture
    // from these. FFmpeg holds the buffer as its reference and never writes it again; writing it through the picture it
    // gave is outside its API's contract, which is why only the decoders above use it. False when no picture of that
    // size and format is kept.
    bool rewrite_last(u32 w, u32 h, const u8* y, const u8* u, const u8* v)
    {
#if PSPOCKET_HAVE_FFMPEG
        if (!last_ || !last_->data[0] || last_->width != static_cast<int>(w) || last_->height != static_cast<int>(h))
            return false;
        if (last_->format != AV_PIX_FMT_YUV420P && last_->format != AV_PIX_FMT_YUVJ420P) return false;
        write_planes(last_, w, h, y, u, v);
        return true;
#else
        (void)w, (void)h, (void)y, (void)u, (void)v;
        return false;
#endif
    }

    // Keeps a reference to each picture FFmpeg gives, by its access unit's index, until forget_all_but(): for a core
    // whose SPU program predicts from its frame buffers as they are, whatever they hold (D-920), the caller rewrites the
    // pictures FFmpeg predicts from (rewrite()). Only for decoders whose references are the pictures they gave (each
    // given as soon as it is decoded: VC-1's with AV_CODEC_FLAG_LOW_DELAY); like rewrite_last(), outside the API's
    // contract.
    void keep_pictures(bool on) { keep_pictures_ = on; }

    // Drops the pictures kept but those of access units `a` and `b` (0: none).
    void forget_all_but([[maybe_unused]] u64 a, [[maybe_unused]] u64 b)
    {
#if PSPOCKET_HAVE_FFMPEG
        for (auto it = kept_.begin(); it != kept_.end();) {
            if (it->first == a || it->first == b) {
                ++it;
                continue;
            }
            av_frame_free(&it->second);
            it = kept_.erase(it);
        }
#endif
    }

    // Whether the picture of access unit `au` is kept and holds these planes (planar 4:2:0, no padding, w x h).
    bool holds([[maybe_unused]] u64 au, [[maybe_unused]] u32 w, [[maybe_unused]] u32 h,
        [[maybe_unused]] const u8* y, [[maybe_unused]] const u8* u, [[maybe_unused]] const u8* v) const
    {
#if PSPOCKET_HAVE_FFMPEG
        const auto it = kept_.find(au);
        if (it == kept_.end() || !fits(it->second, w, h)) return false;
        const AVFrame* f = it->second;
        for (u32 r = 0; r < h; r++)
            if (std::memcmp(f->data[0] + r * static_cast<u32>(f->linesize[0]), y + r * w, w)) return false;
        for (u32 r = 0; r < h / 2; r++)
            if (std::memcmp(f->data[1] + r * static_cast<u32>(f->linesize[1]), u + r * (w / 2), w / 2) ||
                std::memcmp(f->data[2] + r * static_cast<u32>(f->linesize[2]), v + r * (w / 2), w / 2))
                return false;
        return true;
#else
        return false;
#endif
    }

    // The kept picture of access unit `au` rewritten as these planes. False when it is not kept (or of another size).
    bool rewrite([[maybe_unused]] u64 au, [[maybe_unused]] u32 w, [[maybe_unused]] u32 h,
        [[maybe_unused]] const u8* y, [[maybe_unused]] const u8* u, [[maybe_unused]] const u8* v)
    {
#if PSPOCKET_HAVE_FFMPEG
        const auto it = kept_.find(au);
        if (it == kept_.end() || !fits(it->second, w, h)) return false;
        write_planes(it->second, w, h, y, u, v);
        return true;
#else
        return false;
#endif
    }

private:
#if PSPOCKET_HAVE_FFMPEG
    static bool fits(const AVFrame* f, u32 w, u32 h)
    {
        return f && f->data[0] && f->width == static_cast<int>(w) && f->height == static_cast<int>(h) &&
               (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P);
    }
    static void write_planes(AVFrame* f, u32 w, u32 h, const u8* y, const u8* u, const u8* v)
    {
        for (u32 r = 0; r < h; r++) std::memcpy(f->data[0] + r * static_cast<u32>(f->linesize[0]), y + r * w, w);
        for (u32 r = 0; r < h / 2; r++) {
            std::memcpy(f->data[1] + r * static_cast<u32>(f->linesize[1]), u + r * (w / 2), w / 2);
            std::memcpy(f->data[2] + r * static_cast<u32>(f->linesize[2]), v + r * (w / 2), w / 2);
        }
    }

    // FFmpeg's buffers, the next one filled as prefill() asked.
    static int get_buffer(AVCodecContext* av, AVFrame* f, int flags)
    {
        const int r = avcodec_default_get_buffer2(av, f, flags);
        auto* self = static_cast<FfDecoder*>(av->opaque);
        if (r < 0 || !self || !self->prefill_w_) return r;
        const u32 w = self->prefill_w_, h = self->prefill_h_;
        self->prefill_w_ = 0;
        if ((f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) || f->width < static_cast<int>(w) ||
            f->height < static_cast<int>(h) || self->prefill_y_.size() < w * h)
            return r;
        for (u32 row = 0; row < h; row++)
            std::memcpy(f->data[0] + row * static_cast<u32>(f->linesize[0]), self->prefill_y_.data() + row * w, w);
        const u32 cw = w / 2;
        for (u32 row = 0; row < h / 2; row++) {
            std::memcpy(f->data[1] + row * static_cast<u32>(f->linesize[1]), self->prefill_u_.data() + row * cw, cw);
            std::memcpy(f->data[2] + row * static_cast<u32>(f->linesize[2]), self->prefill_v_.data() + row * cw, cw);
        }
        return r;
    }

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
            if (keep_last_) {
                if (!last_) last_ = av_frame_alloc();
                av_frame_unref(last_);
                if (av_frame_ref(last_, f) < 0) av_frame_unref(last_);
            }
            if (keep_pictures_) {
                AVFrame*& k = kept_[static_cast<u64>(reinterpret_cast<uintptr_t>(f->opaque))];
                av_frame_free(&k);
                k = av_frame_clone(f);
            }
            av_frame_unref(f);
        }
        av_frame_free(&f);
    }

    AVCodecContext* av_ = nullptr;
    AVFrame* last_ = nullptr;
    std::map<u64, AVFrame*> kept_;
#endif
    bool keep_last_ = false, keep_pictures_ = false;
    u32 prefill_w_ = 0, prefill_h_ = 0;
    std::vector<u8> prefill_y_, prefill_u_, prefill_v_;
};

} // namespace pspocket::hle::vdec
