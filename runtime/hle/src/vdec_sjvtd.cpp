// libsjvtd, the JVT (H.264) core Sony's libvdec loads for CELL_VDEC_CODEC_TYPE_JVT, pspocket's own (D-920 part 8):
// answering as Sony's module does (firmware 4.92), compared call for call in tests/hle/test_vdec_sjvtd_compare.cpp.
// Sony's module is a PPU library over SPU tasks on the caller's SPURS taskset; pspocket does the tasks' work on the
// host: the stream's syntax read as Sony's decoder records it (vdec_h264.hpp's readers), the samples decoded by FFmpeg
// (H.264's reconstruction is exact by the standard) into Sony's frame buffers, laid out as Sony's are, and the pictures
// given out when its decoded picture buffer outputs them.
//
// The instance's memory (GetMemorySize's size, made of the picture's size, the frame buffers' count and two flags):
// the PPU side's state first (its first word the decoder's, its second the existence flag GetExistenceFlag reads), the
// SPU side's areas, then the frame buffers: each laid out in 16-sample strips, the luma's lines with 32 lines of edge
// above and below (never written) and a strip each side (a reference picture's lines repeated), chroma half that, Cb
// and Cr in each strip's halves. The records GetSideInfoSeq and GetSideInfoPic give, the decoded picture buffer's
// size and output (the HRD's times when the stream has them), the decode mode, CancelDecode and the errors are the
// program's as read in the module and compared (D-920 part 8 in docs/decisions.md).
//
// Nothing here is Sony's code or data (CLAUDE.md rule 1): the behaviour is written from what the module does.

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "vdec_core_common.hpp"
#include "vdec_h264.hpp"
#include "vdec_layout.hpp"
#include "vdec_score.hpp"

#if PSPOCKET_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}
#endif

namespace pspocket::hle::vdec_core {
namespace {

using namespace vdec::h264;
using vdec::align;

constexpr const char* kChan = "sjvtd";
constexpr u32 kPrefix = 0x80615800;

s32 fail(u32 low) { return score::code(kPrefix, low); }

// ---- the memory

// The largest picture of each level (the H.264 level each stands for: 1, 1.1 to 2, 2.1, 2.2 and 3, 3.1, 3.2, 4 to 5.1;
// 16 is level 1b; 0 any).
bool level_size(u32 level, u32& w, u32& h)
{
    constexpr u32 kW[] = {1920, 176, 352, 352, 352, 352, 352, 720, 720, 1280, 1280, 1920, 1920, 1920, 1920, 1920, 176};
    constexpr u32 kH[] = {1088, 144, 288, 288, 288, 288, 576, 576, 576, 720, 1024, 1088, 1088, 1088, 1088, 1088, 144};
    if (level > 16) return false;
    w = kW[level];
    h = kH[level];
    return true;
}

// A frame buffer's bytes (the lines to 32 for its size, 64 lines of edge, two strips of edge, 0x80 after it) and the
// motion data each picture keeps beside it.
u32 frame_bytes(u32 w, u32 h)
{
    const u32 mbw = (w + 15) / 16, mbh = (h + 15) / 16;
    return ((((mbh * 16 + 31) & ~31u) + 64) * (3 * (mbw * 16 + 32)) / 2 + 0x80 + 0x7F) & ~0x7Fu;
}
u32 motion_bytes(u32 w, u32 h)
{
    const u32 mbw = (w + 15) / 16, mbh = (h + 15) / 16;
    return ((((mbw << 8) + ((mbw + 0x7F) & ~0x7Fu)) * ((mbh + 1) & ~1u)) + 0x7F) & ~0x7Fu;
}

// The areas before the frame buffers: their fixed part, 0x100 a macroblock row, and 0x24A80 for the first flag's
// option and for the second flag's default (both its options). The size counts them as they are; the frame buffers
// start at the next 128 bytes, past one more area.
u32 head_bytes(u32 h, bool f5, bool f6_both)
{
    const u32 mbh = (h + 15) / 16;
    return 0x95A50 + 0x100 * mbh + 0x24A80 * ((f5 ? 1 : 0) + (f6_both ? 1 : 0));
}

// The memory an instance asks for: the head, the frame buffers (two more than the count) with their motion data, a
// table a macroblock column and one a macroblock, and the rest; without the caller's taskset (GetMemorySize, or
// GetMemorySize2's parameters with none), room for one of its own as well (a CellSpursTaskset2, 0x2900 bytes).
constexpr u32 kTasksetBytes = 0x2900;
u32 memory_for(u32 w, u32 h, u32 frames, bool f5, bool f6_both, bool own_taskset = true)
{
    const u32 mbw = (w + 15) / 16, mbh = (h + 15) / 16;
    const u32 columns = (mbw * 0x240 + 0x4FF) & ~0x7Fu;
    return head_bytes(h, f5, f6_both) + (frame_bytes(w, h) + motion_bytes(w, h)) * (frames + 2) + columns +
           mbh * mbw * 0x600 + 0x31D00 - (own_taskset ? 0 : kTasksetBytes);
}

// ---- the stream's records

// GetSideInfoSeq's structure (the sequence parameter set as Sony's decoder records it, 0x7E0 bytes) and a picture's
// GetSideInfoPic block (0x188 bytes a field).
constexpr u32 kSeqBytes = 0x7E0, kPicBytes = 0x188;

// A sequence parameter set as Sony's decoder records it. Its SPU task reads each set into one record it keeps from set
// to set, so a field the syntax leaves out (the VUI's, past a flag of 0) keeps the value of the last set that had it;
// in a new instance the record starts as the SPU's local store happened to be (zeros here).
struct HrdRecord {
    u32 count = 0;
    std::array<std::array<u32, 3>, 32> cpb{}; // each CPB's bit rate, cbr_flag and size (E.2.2's BitRate, CpbSize)
};

struct SeqRecord {
    u32 profile = 0, constraints = 0, level = 0, id = 0;
    u32 log2_max_frame_num = 4, poc_type = 0, log2_max_poc_lsb = 4, delta_always_zero = 0;
    u32 cycle = 0;
    s32 offset_non_ref = 0, offset_top_bottom = 0;
    std::array<s32, 255> offsets{};
    u32 max_num_ref_frames = 0, gaps = 0, width_mbs = 0, height_map_units = 0, frame_mbs_only = 0, mbaff = 0;
    u32 direct_8x8 = 0;
    u32 crop = 0, crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    u32 vui = 0;
    u32 aspect = 0, idc = 0, sar_w = 0, sar_h = 0;
    u32 overscan = 0, overscan_appropriate = 0;
    u32 video_signal = 0, video_format = 0, full_range = 0;
    u32 colour = 0, primaries = 0, transfer = 0, matrix = 0;
    u32 chroma_loc = 0, chroma_top = 0, chroma_bottom = 0;
    u32 timing = 0, units_in_tick = 0, time_scale = 0, fixed_rate = 0;
    u32 nal_hrd = 0, vcl_hrd = 0;
    HrdRecord nal, vcl;
    u32 low_delay = 0, pic_struct = 0;
    u32 restriction = 0, num_reorder = 0, max_dec_buffering = 0;
    bool vui_seen = false; // a set with a VUI was read (the VUI's values are reported from then on)
};

void read_hrd(Rbsp& b, HrdRecord& h)
{
    h.count = b.ue() + 1;
    const u32 rate_scale = b.bits(4), size_scale = b.bits(4);
    for (u32 i = 0; i < h.count && i < 32 && !b.over(); i++) {
        const u32 rate = b.ue(), size = b.ue();
        const u32 cbr = b.bit();
        h.cpb[i] = {(rate + 1) << (6 + rate_scale), cbr, (size + 1) << (4 + size_scale)};
    }
    b.bits(20); // the four lengths
}

// seq_parameter_set_data() and its VUI (7.3.2.1.1, E.1.1) into the record `r`: false when the set is not one to keep.
bool read_record(Rbsp& b, SeqRecord& r)
{
    r.profile = b.bits(8);
    r.constraints = b.bits(8);
    r.level = b.bits(8);
    r.id = b.ue();
    if (high_profile(r.profile)) {
        const u32 chroma = b.ue();
        if (chroma == 3) b.bit();
        b.ue();
        b.ue();
        b.bit();
        if (b.flag())
            for (u32 i = 0; i < (chroma != 3 ? 8u : 12u); i++)
                if (b.flag()) scaling_list(b, i < 6 ? 16 : 64);
    }
    r.log2_max_frame_num = b.ue() + 4;
    r.poc_type = b.ue();
    r.log2_max_poc_lsb = 4; // (delta_pic_order_always_zero_flag stays as the last set of type 1 had it)
    r.offset_non_ref = r.offset_top_bottom = 0;
    r.cycle = 0;
    r.offsets.fill(0);
    if (r.poc_type == 0) {
        r.log2_max_poc_lsb = b.ue() + 4;
    } else if (r.poc_type == 1) {
        r.delta_always_zero = b.bit();
        r.offset_non_ref = b.se();
        r.offset_top_bottom = b.se();
        r.cycle = b.ue();
        for (u32 i = 0; i < r.cycle && i < 255 && !b.over(); i++) r.offsets[i] = b.se();
    }
    r.max_num_ref_frames = b.ue();
    r.gaps = b.bit();
    r.width_mbs = b.ue() + 1;
    r.height_map_units = b.ue() + 1;
    r.frame_mbs_only = b.bit();
    r.mbaff = r.frame_mbs_only ? 0 : b.bit();
    r.direct_8x8 = b.bit();
    r.crop = b.bit();
    if (r.crop) {
        r.crop_left = b.ue();
        r.crop_right = b.ue();
        r.crop_top = b.ue();
        r.crop_bottom = b.ue();
    }
    r.vui = b.bit();
    if (r.vui) {
        r.vui_seen = true;
        r.aspect = b.bit();
        if (r.aspect) {
            r.idc = b.bits(8);
            if (r.idc == 255) {
                r.sar_w = b.bits(16);
                r.sar_h = b.bits(16);
            }
        }
        r.overscan = b.bit();
        if (r.overscan) r.overscan_appropriate = b.bit();
        r.video_signal = b.bit();
        r.colour = 0;
        if (r.video_signal) {
            r.video_format = b.bits(3);
            r.full_range = b.bit();
            r.colour = b.bit();
            if (r.colour) {
                r.primaries = b.bits(8);
                r.transfer = b.bits(8);
                r.matrix = b.bits(8);
            }
        }
        r.chroma_loc = b.bit();
        if (r.chroma_loc) {
            r.chroma_top = b.ue();
            r.chroma_bottom = b.ue();
        }
        r.timing = b.bit();
        r.fixed_rate = 0;
        if (r.timing) {
            r.units_in_tick = b.bits(32);
            r.time_scale = b.bits(32);
            r.fixed_rate = b.bit();
        }
        r.nal_hrd = b.bit();
        if (r.nal_hrd) read_hrd(b, r.nal);
        r.vcl_hrd = b.bit();
        if (r.vcl_hrd) read_hrd(b, r.vcl);
        if (r.nal_hrd || r.vcl_hrd) r.low_delay = b.bit();
        r.pic_struct = b.bit();
        r.restriction = b.bit();
        if (r.restriction) {
            b.bit();
            b.ue();
            b.ue();
            b.ue();
            b.ue();
            r.num_reorder = b.ue();
            r.max_dec_buffering = b.ue();
        }
    }
    return !b.over();
}

// GetSideInfoSeq's structure of a record: the words written, each (offset, value). The VUI's fields are the record's,
// whether the set had a VUI or not (a set without one leaves the last VUI's); the values behind a flag only once a set
// had a VUI, the timing's when it is fixed.
std::vector<std::pair<u32, u32>> record_words(const SeqRecord& r)
{
    std::vector<std::pair<u32, u32>> w;
    const auto put = [&](u32 off, u32 v) { w.emplace_back(off, v); };
    const bool rate = r.timing && r.fixed_rate;
    put(0x00, 1);
    put(0x04, 1);
    put(0x08, 1);
    put(0x0C, r.crop);
    put(0x10, rate ? 1 : 0);
    put(0x14, r.width_mbs * 16);
    put(0x18, r.height_map_units * (r.frame_mbs_only ? 1 : 2) * 16);
    put(0x1C, r.aspect);
    put(0x20, r.idc);
    put(0x24, r.sar_w);
    put(0x28, r.sar_h);
    put(0x2C, r.vcl_hrd ? r.vcl.count : 0);
    put(0x30, r.nal_hrd ? r.nal.count : 0);
    for (const auto& [on, h, at] : {std::tuple{r.vcl_hrd, &r.vcl, 0x34u}, std::tuple{r.nal_hrd, &r.nal, 0x1B4u}})
        if (on)
            for (u32 i = 0; i < h->count && i < 32; i++)
                for (u32 k = 0; k < 3; k++) put(at + 12 * i + 4 * k, h->cpb[i][k]);
    put(0x334, r.overscan);
    put(0x33C, r.video_signal);
    put(0x348, r.colour);
    put(0x358, r.chroma_loc);
    if (r.vui_seen) {
        put(0x338, r.overscan_appropriate);
        put(0x340, r.video_format);
        put(0x344, r.full_range);
        put(0x34C, r.primaries);
        put(0x350, r.transfer);
        put(0x354, r.matrix);
        put(0x35C, r.chroma_top);
        put(0x360, r.chroma_bottom);
    }
    put(0x364, (r.constraints >> 7) & 1);
    put(0x368, (r.constraints >> 6) & 1);
    put(0x36C, (r.constraints >> 5) & 1);
    put(0x370, r.profile);
    put(0x374, r.level);
    put(0x378, r.log2_max_frame_num - 4);
    put(0x37C, r.poc_type);
    put(0x380, r.log2_max_poc_lsb - 4);
    put(0x384, r.delta_always_zero);
    put(0x388, r.cycle);
    put(0x38C, static_cast<u32>(r.offset_non_ref));
    put(0x390, static_cast<u32>(r.offset_top_bottom));
    for (u32 i = 0; i < 255; i++) put(0x394 + 4 * i, static_cast<u32>(r.offsets[i]));
    put(0x790, r.max_num_ref_frames);
    put(0x794, r.gaps);
    put(0x798, r.frame_mbs_only);
    put(0x79C, r.mbaff);
    put(0x7A0, r.direct_8x8);
    put(0x7A4, r.vui);
    put(0x7A8, r.num_reorder);
    put(0x7AC, r.max_dec_buffering);
    if (r.vui_seen) {
        put(0x7B0, r.timing);
        put(0x7B4, r.fixed_rate);
        put(0x7B8, r.nal_hrd);
        put(0x7BC, r.vcl_hrd);
        put(0x7C0, r.low_delay);
    }
    if (r.crop) {
        put(0x7C4, r.crop_left);
        put(0x7C8, r.crop_top);
        put(0x7CC, r.crop_right);
        put(0x7D0, r.crop_bottom);
    }
    if (rate) {
        put(0x7D4, 0);
        put(0x7D8, r.units_in_tick);
        put(0x7DC, r.time_scale);
    }
    return w;
}

// ---- the decoder

#if PSPOCKET_HAVE_FFMPEG
// FFmpeg's H.264 decoder as the source of the samples: each picture's buffer captured when FFmpeg takes it, so that
// its samples are read when its access unit is decoded, whatever FFmpeg's own output order.
class Samples {
public:
    ~Samples() { close(); }
    // `deblock` false: the loop filter left out (the instance's first flag 0, libvdec's disableDeblockingFilter).
    bool open(bool deblock)
    {
        close();
        const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!dec) return false;
        av_ = avcodec_alloc_context3(dec);
        av_->thread_count = 1;
        av_->apply_cropping = 0;
        av_->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
        if (!deblock) av_->skip_loop_filter = AVDISCARD_ALL;
        av_->opaque = this;
        av_->get_buffer2 = get_buffer;
        if (avcodec_open2(av_, dec, nullptr) < 0) {
            avcodec_free_context(&av_);
            return false;
        }
        frame_ = av_frame_alloc();
        out_ = av_frame_alloc();
        return true;
    }
    void close()
    {
        if (frame_) av_frame_free(&frame_);
        if (out_) av_frame_free(&out_);
        avcodec_free_context(&av_);
    }
    bool is_open() const { return av_ != nullptr; }
    void reset()
    {
        if (av_) avcodec_flush_buffers(av_);
        if (frame_) av_frame_unref(frame_);
        captured_ = false;
    }
    // Decodes one access unit; whether a new picture's buffer was taken (its samples then in planes()).
    bool decode(const u8* data, u32 size, bool skip_nonref)
    {
        if (!av_) return false;
        captured_ = false;
        std::vector<u8> copy(size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        std::memcpy(copy.data(), data, size);
        AVPacket* pkt = av_packet_alloc();
        pkt->data = copy.data();
        pkt->size = static_cast<int>(size);
        av_->skip_frame = skip_nonref ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
        avcodec_send_packet(av_, pkt);
        av_packet_free(&pkt);
        while (avcodec_receive_frame(av_, out_) == 0) av_frame_unref(out_);
        return captured_;
    }
    // The last picture FFmpeg decoded into: its planes at w x h (planar 4:2:0, no padding).
    bool planes(u32 w, u32 h, std::vector<u8>& y, std::vector<u8>& u, std::vector<u8>& v) const
    {
        if (!frame_ || !frame_->data[0]) return false;
        if (frame_->format != AV_PIX_FMT_YUV420P && frame_->format != AV_PIX_FMT_YUVJ420P) return false;
        if (frame_->width < static_cast<int>(w) || frame_->height < static_cast<int>(h)) return false;
        y.resize(w * h);
        u.resize(w * h / 4);
        v.resize(w * h / 4);
        for (u32 r = 0; r < h; r++)
            std::memcpy(&y[r * w], frame_->data[0] + r * static_cast<u32>(frame_->linesize[0]), w);
        for (u32 r = 0; r < h / 2; r++) {
            std::memcpy(&u[r * w / 2], frame_->data[1] + r * static_cast<u32>(frame_->linesize[1]), w / 2);
            std::memcpy(&v[r * w / 2], frame_->data[2] + r * static_cast<u32>(frame_->linesize[2]), w / 2);
        }
        return true;
    }

private:
    static int get_buffer(AVCodecContext* av, AVFrame* f, int flags)
    {
        const int r = avcodec_default_get_buffer2(av, f, flags);
        auto* self = static_cast<Samples*>(av->opaque);
        if (r >= 0 && self && self->frame_) {
            av_frame_unref(self->frame_);
            if (av_frame_ref(self->frame_, f) >= 0) self->captured_ = true;
        }
        return r;
    }
    AVCodecContext* av_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* out_ = nullptr;
    bool captured_ = false;
};
#endif

// A picture in the decoded picture buffer (a frame, or a field pair).
struct Pic {
    int buffer = -1;
    s32 poc = 0; // the frame's: its fields' smaller
    bool reference = true, long_term = false, output_done = false, idr = false, mmco5 = false;
    u32 frame_num = 0;
    s32 frame_num_wrap = 0;
    u32 long_term_frame_idx = 0;
    std::vector<Mmco> mmco;
    u32 width = 0, height = 0;
    u32 format = 0x40000004;
    u32 fields = 1;             // the side information's blocks (two for a field pair)
    bool field_pic = false, first_bottom = false;
    bool lone = false;          // a field output on its own (the HRD's times)
    bool complete = true;       // false while a first field waits for its second
    bool waiting = false;       // output before it was complete: given out when it is
    bool timed = false;         // output at its time (the HRD's, C.4.5.2)
    s64 output_time = 0;
    u8 side[2][kPicBytes] = {};
    u8 aux[2][16] = {};
    u32 status = 0;
};
using PicPtr = std::shared_ptr<Pic>;

struct Sjvtd {
    std::mutex m;
    u32 handle = 0, level = 0, max_w = 0, max_h = 0;
    u32 frames = 16;    // the decoded picture buffer's limit (the parameters' frame count)
    u32 buffers = 18;   // the frame buffers
    u32 frame0 = 0, frame_stride = 0;
    bool dead = false;
    bool pad = true;    // the parameters' first flag (libvdec's !disableDeblockingFilter): the loop filter run and
                        // the reference pictures' edge strips padded; 0: neither
    bool refuse = false; // the second flag 1: every access unit refused (Sony's module answers it so, D-920 part 8)
    u32 mode = 0;       // SetDecodeMode's word: 0xFF passes the non-reference pictures over (given out undecoded)
    bool lost_refs = false; // CancelDecode dropped the references: P and B pictures warn until an IDR picture
    bool restart = false;   // CancelDecode's: the next picture starts the decoding again (side information 0x170)
    u8 aux_next[16] = {}; // SetAuxData's, for the next access unit
    std::array<std::optional<Sps>, 32> sps;
    std::array<std::optional<Pps>, 256> pps;
    PocState poc;
    std::vector<PicPtr> dpb;
    u32 dpb_size = 16;
    u32 capacity = 0;            // the first sequence's buffer size: no later one may need more
    bool started = false;        // a sequence started since Initialize (its buffer's size then set)
    PicPtr pending;              // a first field waiting for its second
    bool bp_seen = false;        // the HRD's clock (C.1.2): the last buffering period's removal time
    s64 bp_time = 0;
    bool timing_mode = false;    // pictures output at their times (a picture timing message since Initialize)
    s64 now = 0;                 // the last picture's removal time
    std::vector<PicPtr> lone;    // second fields of frames whose first was output alone, to output at their times
    std::optional<u32> recovery; // the frame_num of the picture a recovery point SEI named
    std::deque<PicPtr> out;      // output, not yet released (GetPicture takes them in turn)
    usize got = 0;               // the pictures of `out` GetPicture gave
    PicPtr current;              // GetPicture's last
    bool flushed = false;        // FlushPicture's mark (GetNumPicture answers 0x..22)
    SeqRecord parse;                       // the record the sets are read into (kept from set to set)
    std::array<std::optional<SeqRecord>, 32> records; // each set's, as read
    std::optional<SeqRecord> active;       // the sequence of the last picture decoded (GetSideInfoSeq's)
    vdec::Plan plan;
#if PSPOCKET_HAVE_FFMPEG
    Samples ff;
#endif
};

score::Instances<Sjvtd>& sjvtds() { return score::Instances<Sjvtd>::of_process(kChan); }

std::shared_ptr<Sjvtd> instance(u32 handle, const char* call)
{
    std::shared_ptr<Sjvtd> x = sjvtds().find(handle);
    if (!x) PS_LOG_ERROR(kChan, "{} on {:#x}, which is no instance", call, handle);
    return x;
}

// ---- the decoded picture buffer (C.4.5.3)

// The size for a sequence: the VUI's max_dec_frame_buffering when the stream gives one, else the level's MaxDpbMbs
// over the frame's macroblocks; at most 16, and at most the instance's frame count.
u32 dpb_size_of(const Sjvtd& x, const Sps& s)
{
    u32 n = 0;
    if (s.vui_present && s.vui.bitstream_restriction && s.vui.max_dec_frame_buffering)
        n = s.vui.max_dec_frame_buffering;
    if (!n) {
        u32 mbs = 0;
        switch (s.level_idc) {
        case 9:
        case 10: mbs = 396; break;
        case 11: mbs = (s.constraints & 0x10) ? 396 : 900; break;
        case 12:
        case 13:
        case 20: mbs = 2376; break;
        case 21: mbs = 4752; break;
        case 22:
        case 30: mbs = 8100; break;
        case 31: mbs = 18000; break;
        case 32: mbs = 20480; break;
        case 40:
        case 41: mbs = 32768; break;
        case 42: mbs = 34816; break;
        case 50: mbs = 110400; break;
        default: mbs = 184320; break;
        }
        const u32 frame_mbs = s.width_mbs * s.frame_height_mbs();
        n = frame_mbs ? mbs / frame_mbs : 16;
    }
    return std::clamp<u32>(n, 1, std::min<u32>(16, x.frames));
}

void remove(Sjvtd& x, const PicPtr& p) { std::erase(x.dpb, p); }

// A picture given out: to GetPicture's queue, or when its second field comes.
void give(Sjvtd& x, const PicPtr& p)
{
    if (p->complete) {
        x.out.push_back(p);
    } else if (p->timed) {
        // A first field at its output time before its second: a picture of one field.
        p->lone = true;
        p->format = p->first_bottom ? 0x40001410 : 0x40000410;
        x.out.push_back(p);
    } else {
        p->waiting = true;
    }
}

void output(Sjvtd& x, const PicPtr& p)
{
    p->output_done = true;
    give(x, p);
    if (!p->reference) remove(x, p);
}

PicPtr smallest_poc(const Sjvtd& x)
{
    PicPtr best;
    for (const PicPtr& p : x.dpb)
        if (!p->output_done && (!best || p->poc < best->poc)) best = p;
    return best;
}

void flush_dpb(Sjvtd& x)
{
    for (const PicPtr& p : x.lone) {
        p->output_done = true;
        x.out.push_back(p);
    }
    x.lone.clear();
    for (const PicPtr& p : x.dpb) p->reference = p->long_term = false;
    for (;;) {
        // The pictures of HRD times by their times, the others by their order counts (C.4.5.2, C.4.5.3).
        PicPtr best;
        for (const PicPtr& p : x.dpb) {
            if (p->output_done) continue;
            if (!best || (p->timed && best->timed && p->output_time < best->output_time) ||
                (!(p->timed && best->timed && p->output_time != best->output_time) && p->poc < best->poc))
                best = p;
        }
        if (!best) break;
        output(x, best);
    }
    x.dpb.clear();
}

void unmark(Sjvtd& x, const PicPtr& p)
{
    p->reference = p->long_term = false;
    if (p->output_done) remove(x, p);
}

// The short-term references' FrameNumWrap (8.2.4.1) for a picture of `frame_num`.
void number(Sjvtd& x, const Sps& s, u32 frame_num)
{
    for (const PicPtr& p : x.dpb)
        if (p->reference && !p->long_term)
            p->frame_num_wrap = p->frame_num > frame_num
                                    ? static_cast<s32>(p->frame_num) - static_cast<s32>(s.max_frame_num())
                                    : static_cast<s32>(p->frame_num);
}

void sliding_window(Sjvtd& x, const Sps& s, u32 frame_num)
{
    number(x, s, frame_num);
    const u32 max_refs = std::max<u32>(1, s.max_num_ref_frames);
    for (;;) {
        u32 refs = 0;
        PicPtr oldest;
        for (const PicPtr& p : x.dpb) {
            if (!p->reference) continue;
            refs++;
            if (!p->long_term && (!oldest || p->frame_num_wrap < oldest->frame_num_wrap)) oldest = p;
        }
        if (refs < max_refs || !oldest) return;
        unmark(x, oldest);
    }
}

void adaptive_marking(Sjvtd& x, const PicPtr& p)
{
    const s32 cur = static_cast<s32>(p->frame_num);
    for (const Mmco& m : p->mmco) {
        const std::vector<PicPtr> refs = x.dpb;
        switch (m.op) {
        case 1: {
            const s32 num = cur - static_cast<s32>(m.difference_of_pic_nums_minus1 + 1);
            for (const PicPtr& q : refs)
                if (q->reference && !q->long_term && q->frame_num_wrap == num) unmark(x, q);
            break;
        }
        case 2:
            for (const PicPtr& q : refs)
                if (q->reference && q->long_term && q->long_term_frame_idx == m.long_term_pic_num) unmark(x, q);
            break;
        case 3: {
            const s32 num = cur - static_cast<s32>(m.difference_of_pic_nums_minus1 + 1);
            for (const PicPtr& q : refs)
                if (q->reference && q->long_term && q->long_term_frame_idx == m.long_term_frame_idx) unmark(x, q);
            for (const PicPtr& q : x.dpb)
                if (q->reference && !q->long_term && q->frame_num_wrap == num) {
                    q->long_term = true;
                    q->long_term_frame_idx = m.long_term_frame_idx;
                }
            break;
        }
        case 4:
            for (const PicPtr& q : refs)
                if (q->reference && q->long_term && q->long_term_frame_idx + 1 > m.max_long_term_frame_idx_plus1)
                    unmark(x, q);
            break;
        case 5:
            for (const PicPtr& q : refs) unmark(x, q);
            p->mmco5 = true;
            break;
        case 6:
            for (const PicPtr& q : refs)
                if (q->reference && q->long_term && q->long_term_frame_idx == m.long_term_frame_idx) unmark(x, q);
            p->long_term = true;
            p->long_term_frame_idx = m.long_term_frame_idx;
            break;
        default: break;
        }
    }
}

// The picture decoded joins the buffer (C.4.5.3): an IDR picture or one of memory management operation 5 outputs
// everything before it; a non-reference picture with a smaller count than every one waiting is output at once; else
// the buffer outputs its smallest counts while it is full.
void store(Sjvtd& x, const Sps& s, const PicPtr& p, bool no_output_of_prior_pics)
{
    if (p->idr || p->mmco5) {
        if (no_output_of_prior_pics && p->idr) {
            x.dpb.clear();
        } else {
            flush_dpb(x);
        }
        if (p->mmco5) adaptive_marking(x, p); // (its other operations: the current picture's long-term index)
    } else if (p->reference) {
        if (p->mmco.empty()) {
            sliding_window(x, s, p->frame_num);
        } else {
            number(x, s, p->frame_num);
            adaptive_marking(x, p);
        }
    }
    if (!p->reference && !p->timed) {
        for (;;) {
            const PicPtr q = smallest_poc(x);
            if (!q || p->poc < q->poc) {
                output(x, p);
                return;
            }
            if (x.dpb.size() < x.dpb_size) break;
            output(x, q);
        }
    }
    while (x.dpb.size() >= x.dpb_size) {
        const PicPtr q = smallest_poc(x);
        if (!q) break;
        output(x, q);
    }
    x.dpb.push_back(p);
}

// The HRD's output (C.4.5.2): at a picture's removal time, every picture whose output time has come, by time.
void output_by_time(Sjvtd& x, s64 now)
{
    for (;;) {
        PicPtr best;
        bool lone = false;
        const auto consider = [&](const PicPtr& p, bool is_lone) {
            if (p->timed && !p->output_done && p->output_time <= now &&
                (!best || p->output_time < best->output_time ||
                    (p->output_time == best->output_time && p->poc < best->poc))) {
                best = p;
                lone = is_lone;
            }
        };
        for (const PicPtr& p : x.dpb) consider(p, false);
        for (const PicPtr& p : x.lone) consider(p, true);
        if (!best) return;
        if (lone) {
            best->output_done = true;
            x.out.push_back(best);
            std::erase(x.lone, best);
        } else {
            output(x, best);
        }
    }
}

// The lowest frame buffer neither in the buffer, nor output and held by the caller.
int free_buffer(const Sjvtd& x)
{
    for (int k = 0; k < static_cast<int>(x.buffers); k++) {
        bool busy = false;
        for (const PicPtr& p : x.dpb) busy |= p->buffer == k;
        for (const PicPtr& p : x.out) busy |= p->buffer == k;
        if (x.pending) busy |= x.pending->buffer == k;
        for (const PicPtr& p : x.lone) busy |= p->buffer == k;
        if (!busy) return k;
    }
    return -1;
}

u32 frame_of(const Sjvtd& x, int buffer) { return x.frame0 + static_cast<u32>(buffer) * x.frame_stride; }

// ---- an access unit

struct Unit {
    u32 flag = 0; // the existence flag: 0x1 a sequence parameter set, 0x2 a picture parameter set, 0x10 an access unit
                  // delimiter, 0x100 a buffering period message, 0x200 a picture timing message
    bool picture = false; // a picture (or field) decoded
};

// pic_timing() (D.1.3) as far as the decoder uses it: the two delays and pic_struct.
struct Timing {
    bool present = false, delays = false, has_struct = false;
    u32 cpb_removal_delay = 0, dpb_output_delay = 0, pic_struct = 0;
};

Timing read_timing(const std::vector<u8>& payload, const Sps& s)
{
    Timing t;
    t.present = true;
    Rbsp b(payload);
    const Vui& v = s.vui;
    if (s.vui_present && (v.nal_hrd || v.vcl_hrd)) {
        const Hrd& h = v.nal_hrd ? v.nal : v.vcl;
        t.delays = true;
        t.cpb_removal_delay = b.bits(h.cpb_removal_delay_length);
        t.dpb_output_delay = b.bits(h.dpb_output_delay_length);
    }
    if (s.vui_present && v.pic_struct_present) {
        t.has_struct = true;
        t.pic_struct = b.bits(4);
    }
    return t;
}

// GetPicture's structure word of a frame by its pic_struct (D.2.2; for a frame without one, by its fields' order
// counts): 0x10 two fields shown, 0x1000 the bottom one first.
u32 frame_format(u32 pic_struct)
{
    switch (pic_struct) {
    case 3: return 0x40000014;
    case 4: return 0x40001014;
    case 5: return 0x40004014;
    case 6: return 0x40005014;
    case 7: return 0x40001004;
    case 8: return 0x40004004;
    default: return 0x40000004;
    }
}

// A frame's pic_struct as the decoder takes it from pic_timing: a field's (1, 2) is none (0) and frame doubling (7).
u32 frame_struct(u32 pic_struct)
{
    if (pic_struct == 1 || pic_struct > 8) return 0;
    return pic_struct == 2 ? 7 : pic_struct;
}

// A field's or frame's block of side information.
void side_block(u8* side, bool idr, bool random_access, u32 types, u32 bits, s32 poc, u32 pic_struct, bool bottom,
    const Sps& s, u32 errors = 0, u32 mode = 0, bool restart = false)
{
    std::memset(side, 0xEE, kPicBytes);
    for (u32 k = 0; k < 5; k++) ps_write32(side, 4 * k, 1);
    ps_write32(side, 0x14, s.cropping ? 1 : 0);
    if (s.cropping) {
        const u32 ux = s.crop_unit_x(), uy = s.crop_unit_y();
        ps_write32(side, 0x18, s.crop_left * ux);
        ps_write32(side, 0x1C, s.crop_top * uy);
        ps_write32(side, 0x20, s.crop_right * ux);
        ps_write32(side, 0x24, s.crop_bottom * uy);
    }
    ps_write32(side, 0x28, errors); // 0x1 a reference missing, 0x80 passed over (the decode mode)
    ps_write32(side, 0x2C, random_access ? 1 : 0);
    const u32 code = (types & 4) ? 4 : (types & 2) ? 2 : 1;
    ps_write32(side, 0x30, code);
    ps_write32(side, 0x114, 0);
    for (u32 o = 0x154; o <= 0x160; o += 4) ps_write32(side, o, 0);
    ps_write32(side, 0x164, bits);
    ps_write32(side, 0x168, mode ? 0x200000 : 0);
    ps_write32(side, 0x16C, idr ? 1 : 0);
    ps_write32(side, 0x170, idr || restart ? 1 : 0);
    ps_write32(side, 0x174, code == 4 ? 3 : code);
    ps_write32(side, 0x178, static_cast<u32>(poc));
    ps_write32(side, 0x17C, 0);
    ps_write32(side, 0x180, pic_struct);
    ps_write32(side, 0x184, bottom ? 1 : 0);
}

// A frame's samples into a frame buffer laid out in strips: its lines (from `first`, every `step`: a field's), with the
// edge strips left and right repeating the first and last columns when `pad` (a reference picture's, unless the
// instance's first flag is 0); the lines of edge above and below are left as they are (Sony's decoder does not pad
// them).
void write_frame(u8* base, u32 frame, const vdec::Plan& p, u32 w, u32 h, const u8* y, const u8* u, const u8* v,
    bool pad, u32 first = 0, u32 step = 1)
{
    const u32 hl = align(h, 16), lines = hl + 2 * p.edge_y, clines = hl / 2 + 2 * p.edge_c;
    const u32 strips = align(w, 16) / 16 + 2, chroma = frame + strips * 16 * lines;
    const u32 cw = w / 2, ch = h / 2;
    for (u32 s = 0; s < strips; s++)
        for (u32 r = first; r < h; r += step) {
            if (!pad && (s == 0 || s == strips - 1)) continue;
            u8* out = base + frame + s * 16 * lines + (r + p.edge_y) * 16;
            for (u32 k = 0; k < 16; k++) {
                const s32 xx = static_cast<s32>(s * 16 + k) - 16;
                out[k] = y[r * w + static_cast<u32>(std::clamp<s32>(xx, 0, static_cast<s32>(w) - 1))];
            }
        }
    for (u32 s = 0; s < strips; s++)
        for (u32 r = first; r < ch; r += step) {
            if (!pad && (s == 0 || s == strips - 1)) continue;
            u8* out = base + chroma + s * 16 * clines + (r + p.edge_c) * 16;
            for (u32 k = 0; k < 8; k++) {
                const s32 xx = static_cast<s32>(s * 8 + k) - 8;
                const u32 x = static_cast<u32>(std::clamp<s32>(xx, 0, static_cast<s32>(cw) - 1));
                out[k] = u[r * cw + x];
                out[8 + k] = v[r * cw + x];
            }
        }
}

u32 decode_au(u8* base, Sjvtd& x, const u8* p, u32 size, Unit& u)
{
    const std::vector<Nal> nals = split_nals({p, size});
    std::optional<SliceHeader> first;
    const Sps* sps = nullptr;
    bool idr = false;
    u32 ref_idc = 0, slice_types = 0, slice_bits = 0;
    std::vector<SeiMessage> seis;
    bool slices = false;
    if (nals.empty()) return 0x8; // no NAL unit: a warning
    for (const Nal& n : nals) {
        Rbsp b = payload_of(n);
        switch (n.type) {
        case NAL_SPS: {
            Rbsp again = b;
            const Sps s = parse_sps(b);
            if (s.valid && s.id < 32) {
                x.sps[s.id] = s;
                if (read_record(again, x.parse)) x.records[s.id] = x.parse;
            }
            u.flag |= 0x1;
            break;
        }
        case NAL_PPS: {
            const Pps q = parse_pps(b);
            if (q.valid && q.id < 256) x.pps[q.id] = q;
            u.flag |= 0x2;
            break;
        }
        case NAL_SEI: {
            for (SeiMessage& m : parse_sei(b.data())) seis.push_back(std::move(m));
            break;
        }
        case NAL_AUD: u.flag |= 0x10; break;
        case NAL_SLICE:
        case NAL_IDR: {
            slices = true;
            const Sps* spss[32] = {};
            const Pps* ppss[256] = {};
            for (u32 k = 0; k < 32; k++) spss[k] = x.sps[k] ? &*x.sps[k] : nullptr;
            for (u32 k = 0; k < 256; k++) ppss[k] = x.pps[k] ? &*x.pps[k] : nullptr;
            const Sps* as = nullptr;
            const SliceHeader h = parse_slice_header(b, n.type == NAL_IDR, n.ref_idc, spss, ppss, &as, nullptr);
            if (!h.valid) break;
            slice_types |= h.is_i() ? 1 : h.is_p() ? 2 : 4;
            slice_bits += static_cast<u32>(n.bytes.size()) * 8;
            if (!first) {
                first = h;
                sps = as;
                idr = n.type == NAL_IDR;
                ref_idc = n.ref_idc;
            }
            break;
        }
        default: break;
        }
    }
    for (const SeiMessage& m : seis) {
        if (m.type == 0) u.flag |= 0x100;
        if (m.type == 1) u.flag |= 0x200;
    }
    // An access unit of parameter sets alone is no picture and no error; one of a delimiter alone, or slices of
    // parameter sets the decoder does not have (since Initialize), is no picture (and the sequence structure then has
    // none).
    if (!slices) return (u.flag & 0x10) ? score::ST_NO_PICTURE : 0;
    if (!first || !sps) {
        x.active.reset();
        return score::ST_NO_PICTURE;
    }
    const Sps& s = *sps;
    Timing timing;
    bool buffering = false;
    std::optional<u32> recovery; // recovery_point's frame_num
    for (const SeiMessage& m : seis) {
        if (m.type == 0) buffering = true;
        if (m.type == 1) timing = read_timing(m.payload, s);
        if (m.type == 6) {
            Rbsp rb(m.payload);
            recovery = (first->frame_num + rb.ue()) % s.max_frame_num();
        }
    }
    // The picture a recovery point names (D.2.8) is a random access point, as an IDR picture is (the side
    // information's third word of its own).
    bool random_access = idr;
    if (recovery) x.recovery = recovery;
    if (x.recovery && *x.recovery == first->frame_num) {
        random_access = true;
        x.recovery.reset();
    }
    const u32 w = s.width(), h = s.height();
    if (w > x.max_w || h > x.max_h) {
        // No frame buffer holds it: the decoder takes nothing of the access unit (busy), having output the pictures
        // before an IDR picture as it would.
        PS_LOG_WARN(kChan, "instance {:#x}: a picture of {}x{}, past the instance's {}x{}: refused", x.handle, w, h,
            x.max_w, x.max_h);
        if (idr && !first->no_output_of_prior_pics) flush_dpb(x);
        return score::ST_BUSY;
    }
    // A sequence's first picture since Initialize sets the buffer's size; the instance's first sequence took the
    // frame buffers for its own, and a later one that needs more stops the decoder.
    if (!x.started) {
        const u32 need = dpb_size_of(x, s);
        if (x.capacity && need > x.capacity) {
            PS_LOG_WARN(kChan, "instance {:#x}: a sequence of a {}-frame buffer after one of {}: the decoder stops",
                x.handle, need, x.capacity);
            return score::ST_DEAD | score::ST_NO_MEMORY_BIT;
        }
        if (!x.capacity) x.capacity = need;
        x.dpb_size = need;
        x.started = true;
    }
    if (x.records[s.id]) x.active = x.records[s.id];
    // The HRD's clock (C.1.2): removal times from the last buffering period's. Once a picture had its times, the
    // pictures are output at theirs (C.4.5.2); one without a timing message is output when decoded.
    if (timing.delays) {
        x.now = x.bp_seen ? x.bp_time + timing.cpb_removal_delay : 0;
        if (buffering || !x.bp_seen) {
            x.bp_time = x.now;
            x.bp_seen = true;
        }
        x.timing_mode = true;
    }
    const bool timed = x.timing_mode;
    const s64 output_time = timing.delays ? x.now + timing.dpb_output_delay : x.now;
    Poc poc = picture_order_count(s, *first, idr, ref_idc, x.poc);
    const bool field = first->field_pic, bottom = first->bottom_field;
    // Memory management operation 5 (8.2.1): the picture's order counts made relative to its own (its frame_num 0
    // from then on), and the next picture's counted from it.
    const bool mmco5 = std::any_of(first->mmco.begin(), first->mmco.end(), [](const Mmco& m) { return m.op == 5; });
    if (mmco5) {
        const s32 temp = field ? (bottom ? poc.bottom : poc.top) : std::min(poc.top, poc.bottom);
        poc.top -= temp;
        poc.bottom -= temp;
        x.poc.prev_ref_bottom_field = field && bottom;
        x.poc.prev_ref_top_poc = poc.top;
        x.poc.prev_frame_num = 0;
    }
    if (ref_idc || s.poc_type != 0) x.poc.prev_mmco5 = mmco5;
    const s32 field_poc = bottom ? poc.bottom : poc.top;
    // The picture's structure: pic_timing's pic_struct, else a frame's by its fields' order counts.
    u32 pic_struct = 0;
    if (field) pic_struct = timing.has_struct ? timing.pic_struct : 0;
    else if (timing.has_struct) pic_struct = frame_struct(timing.pic_struct);
    else pic_struct = poc.top < poc.bottom ? 3 : poc.top > poc.bottom ? 4 : 0;
    // The decode mode 0xFF passes a non-reference picture over: it is given out with its frame buffer as it was.
    const bool skip = x.mode == 0xFF && ref_idc == 0;
    if (idr) x.lost_refs = false;
    u32 errors = skip ? 0x80 : 0, status = skip ? 0x80 : 0;
    if (!skip && x.lost_refs && !(slice_types == 1)) {
        errors |= 0x1;
        status |= 0xB00;
    }
    const u32 mode_then = x.mode == 0xFF ? 1 : 0;
    const bool restart = x.restart; // the first picture after CancelDecode
    x.restart = false;
#if PSPOCKET_HAVE_FFMPEG
    if (!x.ff.is_open()) x.ff.open(x.pad);
    x.ff.decode(p, size, skip);
    std::vector<u8> py, pu, pv;
    const bool samples = !skip && x.ff.planes(w, h, py, pu, pv);
#else
    const bool samples = false;
    std::vector<u8> py, pu, pv;
#endif
    // A frame's samples into frame buffer `buffer` (`parity` 0 or 1: one field's lines only, the others as they are).
    const auto put = [&](int buffer, int parity) {
        if (!samples) return;
        const bool pad = ref_idc != 0 && x.pad;
        if (parity < 0) write_frame(base, frame_of(x, buffer), x.plan, w, h, py.data(), pu.data(), pv.data(), pad);
        else write_frame(base, frame_of(x, buffer), x.plan, w, h, py.data(), pu.data(), pv.data(), pad, parity, 2);
    };
    // The second field of a pair: into its first's frame.
    if (field && x.pending && x.pending->first_bottom != bottom && x.pending->frame_num == first->frame_num && !idr) {
        const PicPtr q = x.pending;
        x.pending.reset();
        if (q->lone) {
            // Its first field was output alone: this one is a picture of its own in the same frame buffer.
            auto f = std::make_shared<Pic>(*q);
            f->lone = true;
            f->output_done = false;
            f->format = bottom ? 0x40001410 : 0x40000410;
            f->poc = field_poc;
            f->timed = timed;
            f->output_time = output_time;
            side_block(f->side[0], false, random_access, slice_types, skip ? 0 : slice_bits, field_poc, pic_struct,
                bottom, s, errors, mode_then, restart);
            f->status = status;
            std::memcpy(f->aux[0], x.aux_next, 16);
            put(q->buffer, bottom ? 1 : 0);
            q->complete = true;
            if (timed) {
                x.lone.push_back(f);
                output_by_time(x, x.now);
            } else {
                f->output_done = true;
                x.out.push_back(f);
            }
            u.picture = true;
            return 0;
        }
        side_block(q->side[1], false, random_access, slice_types, skip ? 0 : slice_bits, field_poc, pic_struct, bottom,
            s, errors, mode_then, restart);
        q->status |= status;
        std::memcpy(q->aux[1], x.aux_next, 16);
        q->fields = 2;
        q->poc = std::min(q->poc, field_poc);
        q->complete = true;
        // A pair's structure: its fields' order (both blocks).
        const u32 pair = q->first_bottom ? 4 : 3;
        ps_write32(q->side[0], 0x180, pair);
        ps_write32(q->side[1], 0x180, pair);
        q->format = q->first_bottom ? 0x40001810 : 0x40000810;
        put(q->buffer, -1);
        if (q->waiting) {
            q->waiting = false;
            x.out.push_back(q);
        }
        if (timed) output_by_time(x, x.now);
        u.picture = true;
        return 0;
    }
    if (x.pending) {
        // A first field whose second never came: a picture of its own.
        const PicPtr q = x.pending;
        x.pending.reset();
        q->complete = true;
        if (q->waiting) {
            q->waiting = false;
            x.out.push_back(q);
        }
    }
    auto pic = std::make_shared<Pic>();
    pic->poc = field ? field_poc : poc.frame();
    pic->idr = idr;
    pic->mmco5 = mmco5;
    pic->reference = ref_idc != 0;
    pic->frame_num = mmco5 ? 0 : first->frame_num;
    pic->mmco = first->mmco;
    pic->width = w;
    pic->height = h;
    pic->field_pic = field;
    pic->first_bottom = field && bottom;
    pic->format = field ? (bottom ? 0x40001810 : 0x40000810) : frame_format(pic_struct);
    pic->timed = timed;
    pic->output_time = output_time;
    std::memcpy(pic->aux[0], x.aux_next, 16);
    side_block(pic->side[0], idr, random_access, slice_types, skip ? 0 : slice_bits, field ? field_poc : poc.frame(),
        pic_struct, field && bottom, s, errors, mode_then, restart);
    pic->status = status;
    const int buffer = free_buffer(x);
    if (buffer < 0) return score::ST_BUSY;
    pic->buffer = buffer;
    put(buffer, field ? (bottom ? 1 : 0) : -1);
    if (field) {
        pic->complete = false;
        x.pending = pic;
    }
    store(x, s, pic, first->no_output_of_prior_pics);
    if (timed) output_by_time(x, x.now);
    u.picture = true;
    return skip ? 0x80 : 0; // (a picture passed over answers so)
}

// ---- the API

s32 sjvtdGetMemorySize(PPUContext& ctx, u32 size, u32 level)
{
    if (!writable(ctx, size, 4)) return fail(score::LOW_ARG);
    u8* base = guest_base(ctx);
    u32 w = 0, h = 0;
    if (!level_size(level, w, h)) {
        ps_write32(base, size, 0);
        return fail(score::LOW_SEQ);
    }
    ps_write32(base, size, memory_for(w, h, 16, true, true));
    return lv2::CELL_OK;
}

// The parameters' size and options (types 5 and 6; type 4 the level's largest picture and the defaults): false when
// one is out of its range.
bool parameters(u8* base, u32 param, u32 type, u32 level, u32& w, u32& h, u32& frames, bool& f5, bool& f6_both)
{
    if (!level_size(level, w, h)) return false;
    frames = 16;
    f5 = true;
    f6_both = true;
    if (type == 4) return true;
    const u32 pw = ps_read32(base, param + 12), ph = ps_read32(base, param + 16);
    const u32 p5 = ps_read32(base, param + 20), p6 = ps_read32(base, param + 24), p7 = ps_read32(base, param + 28);
    if (pw != ~0u) {
        if (static_cast<s32>(pw) < 1 || pw > w) return false;
        w = pw;
    }
    if (ph != ~0u) {
        if (static_cast<s32>(ph) < 1 || ph > h) return false;
        h = ph;
    }
    if (p5 != ~0u && p5 > 1) return false;
    if (p6 != ~0u && p6 > 1) return false;
    if (p7 != ~0u && p7 > 16) return false;
    if (p5 != ~0u) f5 = p5 != 0;
    if (p6 != ~0u) f6_both = false;
    if (p7 != ~0u) frames = p7;
    return true;
}

s32 sjvtdGetMemorySize2(PPUContext& ctx, u32 size, u32 level, u32 param)
{
    if (!writable(ctx, size, 4) || !writable(ctx, param, 0x24)) return fail(score::LOW_ARG);
    u8* base = guest_base(ctx);
    const u32 type = ps_read32(base, param);
    u32 w = 0, h = 0, frames = 16;
    bool f5 = true, f6 = true;
    if (type < 4 || type > 6 || !parameters(base, param, type, level, w, h, frames, f5, f6))
        return fail(score::LOW_SEQ);
    ps_write32(base, size, memory_for(w, h, frames, f5, f6, ps_read32(base, param + 8) == 0));
    return lv2::CELL_OK;
}

s32 sjvtdCreateInstance2(PPUContext& ctx, u32 mem, u32 size, u32 level, u32 param)
{
    if (!mem || !writable(ctx, param, 0x24)) return fail(score::LOW_ARG);
    u8* base = guest_base(ctx);
    const u32 type = ps_read32(base, param);
    u32 w = 0, h = 0, frames = 16;
    bool f5 = true, f6 = true;
    if (type < 4 || type > 6 || !parameters(base, param, type, level, w, h, frames, f5, f6))
        return fail(score::LOW_SEQ);
    if (!ps_read32(base, param + 8)) return fail(score::LOW_SEQ); // no taskset
    const u32 spus = ps_read32(base, param + 4);
    if (!spus) return fail(score::LOW_SEQ);
    if (type == 6) {
        const u32 p8 = ps_read32(base, param + 32);
        if (p8 != ~0u && p8 > 1) return fail(score::LOW_SEQ);
    }
    const u32 need = memory_for(w, h, frames, f5, f6, false);
    if (size < need) return fail(score::LOW_SEQ);
    if (!guest_readable(thread_of(ctx), mem, need)) {
        PS_LOG_ERROR(kChan, "CreateInstance2: memory {:#x} ({:#x} bytes) is not guest memory", mem, need);
        return fail(score::LOW_ARG);
    }
    auto x = std::make_shared<Sjvtd>();
    x->handle = mem;
    x->level = level;
    x->max_w = w;
    x->max_h = h;
    x->frames = frames;
    x->buffers = frames + 1;
    x->plan.edge_y = 32;
    x->plan.edge_c = 16;
    x->plan.lines32 = false;
    x->plan.start_inside = true;
    x->frame0 = mem + align(head_bytes(h, f5, f6), 128) + 0x23B80;
    x->pad = f5; // (type 4's parameters are its level's defaults: padded)
    x->refuse = type != 4 && ps_read32(base, param + 24) == 1;
    x->frame_stride = frame_bytes(w, h);
    // The PPU side's state: its first word the decoder's.
    ps_write32(base, mem, mem + 0x50);
    sjvtds().add(mem, std::move(x));
    return lv2::CELL_OK;
}

s32 sjvtdReleaseInstance(PPUContext& ctx, u32 handle)
{
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Sjvtd> x = sjvtds().find(handle);
    if (!x || !sjvtds().remove(handle)) {
        PS_LOG_ERROR(kChan, "ReleaseInstance of {:#x}, which is no instance", handle);
        return fail(score::LOW_ARG);
    }
    ps_write32(guest_base(ctx), handle, 0);
    // (A stopped decoder is released too, and says it had stopped.)
    return x->dead ? fail(score::LOW_FATAL) : lv2::CELL_OK;
}

s32 sjvtdInitialize(PPUContext& ctx, u32 handle)
{
    (void)ctx;
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Sjvtd> x = instance(handle, "Initialize");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->dead) return fail(score::LOW_FATAL);
    x->dpb.clear();
    x->out.clear();
    x->got = 0;
    x->current.reset();
    x->flushed = false;
    x->poc = {};
    x->started = false;
    x->pending.reset();
    x->bp_seen = false;
    x->bp_time = 0;
    x->timing_mode = false;
    x->now = 0;
    x->lone.clear();
    x->recovery.reset();
    // The parameter sets read before are forgotten (the sequence structure keeps the last picture's).
    for (auto& q : x->sps) q.reset();
    for (auto& q : x->pps) q.reset();
    for (auto& q : x->records) q.reset();
#if PSPOCKET_HAVE_FFMPEG
    x->ff.reset();
#endif
    return lv2::CELL_OK;
}

s32 sjvtdSetAuxData(PPUContext& ctx, u32 handle, u32 aux)
{
    if (!handle || !writable(ctx, aux, 16)) return fail(score::LOW_ARG);
    const std::shared_ptr<Sjvtd> x = instance(handle, "SetAuxData");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->dead) return fail(score::LOW_FATAL);
    std::memcpy(x->aux_next, guest_base(ctx) + aux, 16);
    return lv2::CELL_OK;
}

s32 sjvtdDecodePicture(PPUContext& ctx, u32 handle, u32 au, u32 used)
{
    if (!handle || !writable(ctx, au, 8) || !writable(ctx, used, 4)) return fail(score::LOW_ARG);
    const std::shared_ptr<Sjvtd> x = instance(handle, "DecodePicture");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    u8* base = guest_base(ctx);
    if (x->dead) return fail(score::LOW_FATAL);
    const u32 addr = ps_read32(base, au), size = ps_read32(base, au + 4);
    if (size && !guest_readable(thread_of(ctx), addr, size)) {
        PS_LOG_ERROR(kChan, "DecodePicture: {:#x} bytes at {:#x} are not guest memory", size, addr);
        return fail(score::LOW_ARG);
    }
    const std::vector<u8> bytes(base + addr, base + addr + size);
    Unit u;
    const u32 st = x->refuse ? score::ST_SEQ : decode_au(base, *x, bytes.data(), size, u);
    if (st & score::ST_DEAD) x->dead = true;
    ps_write32(base, used, (st & score::ST_BUSY) ? 0 : size); // a refused access unit is taken again
    ps_write32(base, handle + 4, u.flag);
    if (u.picture) x->flushed = false; // (FlushPicture's mark stays until a picture)
    return score::answer(kPrefix, st);
}

s32 sjvtdFlushPicture(PPUContext& ctx, u32 handle)
{
    (void)ctx;
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Sjvtd> x = instance(handle, "FlushPicture");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->dead) return fail(score::LOW_FATAL);
    flush_dpb(*x);
    x->flushed = true;
    return lv2::CELL_OK;
}

s32 sjvtdDiscontinue(PPUContext& ctx, u32 handle)
{
    (void)ctx;
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Sjvtd> x = instance(handle, "Discontinue");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->dead) return fail(score::LOW_FATAL);
    return lv2::CELL_OK;
}

s32 sjvtdCancelDecode(PPUContext& ctx, u32 handle)
{
    (void)ctx;
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Sjvtd> x = instance(handle, "CancelDecode");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->dead) return fail(score::LOW_FATAL);
    // Every picture dropped, given out or not; the pictures after it predict from none until an IDR picture.
    x->dpb.clear();
    x->out.clear();
    x->lone.clear();
    x->pending.reset();
    x->got = 0;
    x->current.reset();
    x->flushed = false;
    x->lost_refs = true;
    x->restart = true;
    return lv2::CELL_OK;
}

s32 sjvtdSetDecodeMode(PPUContext& ctx, u32 handle, u32 mode)
{
    if (!handle || !writable(ctx, mode, 4)) return fail(score::LOW_ARG);
    const std::shared_ptr<Sjvtd> x = instance(handle, "SetDecodeMode");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    const u32 m = ps_read32(guest_base(ctx), mode);
    if (m != 0 && m != 0xFF) return fail(score::LOW_SEQ);
    if (x->dead) return fail(score::LOW_FATAL);
    x->mode = m;
    return lv2::CELL_OK;
}

s32 sjvtdGetNumPicture(PPUContext& ctx, u32 handle, u32 out)
{
    if (!handle || !writable(ctx, out, 4)) return fail(score::LOW_ARG);
    const std::shared_ptr<Sjvtd> x = instance(handle, "GetNumPicture");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    ps_write32(guest_base(ctx), out, static_cast<u32>(x->out.size() - x->got));
    return x->flushed ? score::answer(kPrefix, score::ST_FLUSHED) : lv2::CELL_OK;
}

s32 sjvtdGetPicture(PPUContext& ctx, u32 handle, u32 out)
{
    if (!handle || !writable(ctx, out, 0x24)) return fail(score::LOW_ARG);
    const std::shared_ptr<Sjvtd> x = instance(handle, "GetPicture");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->got >= x->out.size()) {
        x->current.reset();
        return fail(score::LOW_EMPTY);
    }
    const PicPtr p = x->out[x->got++];
    x->current = p;
    u8* base = guest_base(ctx);
    const u32 frame = frame_of(*x, p->buffer);
    // (A bottom field given out alone starts a line down.)
    const u32 down = p->lone && (p->format & 0x1000) ? 16 : 0;
    const u32 luma = frame + vdec::strip_start(x->plan, p->height) + down;
    const u32 cb = frame + vdec::strip_cb(x->plan, p->width, p->height) + down;
    const u32 words[9] = {luma, cb, cb + 8, p->format, p->width, p->height, align(p->height, 16) + 64,
        align(p->height, 16) / 2 + 32, static_cast<u32>(p->buffer)};
    for (u32 k = 0; k < 9; k++) ps_write32(base, out + 4 * k, words[k]);
    return score::picture_answer(kPrefix, p->status);
}

s32 sjvtdReleasePicture(PPUContext& ctx, u32 handle)
{
    (void)ctx;
    if (!handle) return fail(score::LOW_ARG);
    const std::shared_ptr<Sjvtd> x = instance(handle, "ReleasePicture");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    if (x->out.empty() || !x->got) return lv2::CELL_OK; // nothing to release: no error
    x->out.pop_front();
    x->got--;
    return lv2::CELL_OK;
}

s32 sjvtdGetSideInfoPic(PPUContext& ctx, u32 handle, u32 count, u32 first, u32 second)
{
    if (!handle || !writable(ctx, count, 4) || !first || !second) return fail(score::LOW_ARG);
    const std::shared_ptr<Sjvtd> x = instance(handle, "GetSideInfoPic");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    u8* base = guest_base(ctx);
    if (!x->current) {
        ps_write32(base, count, 2); // (the count is written before the answer that there is no picture)
        return fail(score::LOW_EMPTY);
    }
    const PicPtr& p = x->current;
    ps_write32(base, count, p->fields);
    for (u32 k = 0; k < p->fields; k++) {
        const u32 to = k ? second : first;
        for (u32 i = 0; i < kPicBytes; i += 4)
            if (ps_read32(p->side[k], i) != 0xEEEEEEEE) ps_write32(base, to + i, ps_read32(p->side[k], i));
    }
    return lv2::CELL_OK;
}

s32 sjvtdGetAuxData(PPUContext& ctx, u32 handle, u32 count, u32 first, u32 second)
{
    if (!handle || !writable(ctx, count, 4) || !first || !second) return fail(score::LOW_ARG);
    const std::shared_ptr<Sjvtd> x = instance(handle, "GetAuxData");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    u8* base = guest_base(ctx);
    if (!x->current) {
        ps_write32(base, count, 2);
        return fail(score::LOW_EMPTY);
    }
    const PicPtr& p = x->current;
    ps_write32(base, count, p->fields);
    for (u32 k = 0; k < p->fields; k++) std::memcpy(base + (k ? second : first), p->aux[k], 16);
    return lv2::CELL_OK;
}

s32 sjvtdGetSideInfoSeq(PPUContext& ctx, u32 handle, u32 out)
{
    if (!handle || !writable(ctx, out, 0x20)) return fail(score::LOW_ARG);
    const std::shared_ptr<Sjvtd> x = instance(handle, "GetSideInfoSeq");
    if (!x) return fail(score::LOW_ARG);
    std::lock_guard lk(x->m);
    u8* base = guest_base(ctx);
    if (x->dead) return fail(score::LOW_FATAL);
    if (!x->active) {
        for (u32 i = 0; i < 0x14; i += 4) ps_write32(base, out + i, 0);
        return lv2::CELL_OK;
    }
    if (!writable(ctx, out, kSeqBytes)) return fail(score::LOW_ARG);
    for (const auto& [off, v] : record_words(*x->active)) ps_write32(base, out + off, v);
    return lv2::CELL_OK;
}

s32 sjvtdGetExistenceFlag(PPUContext& ctx, u32 handle, u32 out)
{
    if (!handle || !writable(ctx, out, 4)) return fail(score::LOW_ARG);
    u8* base = guest_base(ctx);
    ps_write32(base, out, ps_read32(base, handle + 4)); // (a stopped decoder's too)
    return lv2::CELL_OK;
}

} // namespace

void register_sjvtd(Registry& r)
{
    r.add("libsjvtd", {
        PS_HLE_FUNC(0x9F8CD123, sjvtdGetMemorySize),
        PS_HLE_FUNC(0xA1912CAC, sjvtdGetMemorySize2),
        PS_HLE_FUNC(0xA3683541, sjvtdCreateInstance2),
        PS_HLE_FUNC(0xA41952A4, sjvtdReleaseInstance),
        PS_HLE_FUNC(0x57EA9D70, sjvtdInitialize),
        PS_HLE_FUNC(0x6E949735, sjvtdDecodePicture),
        PS_HLE_FUNC(0x9069BDBB, sjvtdFlushPicture),
        PS_HLE_FUNC(0x549F260D, sjvtdDiscontinue),
        PS_HLE_FUNC(0x7D4375EB, sjvtdCancelDecode),
        PS_HLE_FUNC(0x33CF63E8, sjvtdSetDecodeMode),
        PS_HLE_FUNC(0x3BAB4DB8, sjvtdGetNumPicture),
        PS_HLE_FUNC(0x4558A8E5, sjvtdGetPicture),
        PS_HLE_FUNC(0x437E1EB8, sjvtdReleasePicture),
        PS_HLE_FUNC(0x1B559A96, sjvtdGetSideInfoPic),
        PS_HLE_FUNC(0x98B9408D, sjvtdGetAuxData),
        PS_HLE_FUNC(0x67555D2A, sjvtdGetSideInfoSeq),
        PS_HLE_FUNC(0x68C6AA3F, sjvtdGetExistenceFlag),
        PS_HLE_FUNC(0xBA49D65A, sjvtdSetAuxData),
    });
}

} // namespace pspocket::hle::vdec_core
