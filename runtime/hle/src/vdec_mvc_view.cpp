// libmvcdec's second view (D-933): FFmpeg's H.264 decoder has no MVC (Annex H), so the second view of a stereo stream is
// decoded by a second H.264 decoder given one view's stream made from it. For each access unit that decoder gets the
// base view's picture as decoded (an I picture of I_PCM macroblocks, so its samples are exactly the base view's), then
// the second view's slices as one view's: NAL unit type 1, their parameter sets that decoder's, each slice's reference
// lists spelt out (ref_pic_list_modification) as MVC builds them from the second view's pictures and the base view's
// (the inter-view reference), its reference marking spelt out (memory management operations) as the second view's own
// marking and the base pictures' removal give it, its slice data as it was. What it decodes is then the second view.
// Not done so (the caller is told, and gives the base view's samples): field pictures, other than 4:2:0 8-bit samples,
// long-term references. Where MVC's prediction looks at the inter-view reference's motion (direct prediction from a
// B picture's first list 1 reference when that is the base view's), the I picture has none: not exact.
// Written from ITU-T H.264 (7.3, 8.2.4, H.7, H.8.2); nothing here is Sony's code or data.

#include "vdec_mvc_view.hpp"

#include <algorithm>
#include <cstring>
#include <optional>

#include "pspocket/core/log.hpp"
#include "vdec_h264.hpp"

#if PSPOCKET_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}
#endif

namespace pspocket::hle::vdec::avc {

using namespace h264;

namespace {

constexpr std::string_view kChan = "libmvcdec";

// ---- writing RBSPs

class BitWriter {
public:
    void bit(u32 b)
    {
        cur_ = static_cast<u8>(cur_ << 1 | (b & 1));
        if (++n_ == 8) {
            out_.push_back(cur_);
            cur_ = 0;
            n_ = 0;
        }
    }
    void bits(u32 v, u32 n)
    {
        for (u32 i = n; i-- > 0;) bit((v >> i) & 1);
    }
    void ue(u32 v)
    {
        const u64 x = u64{v} + 1;
        u32 len = 0;
        while ((x >> (len + 1)) != 0) len++;
        for (u32 i = 0; i < len; i++) bit(0);
        for (u32 i = len + 1; i-- > 0;) bit(static_cast<u32>((x >> i) & 1));
    }
    void se(s32 v) { ue(v > 0 ? static_cast<u32>(2 * v - 1) : static_cast<u32>(-2 * static_cast<s64>(v))); }
    bool aligned() const { return n_ == 0; }
    void align_ones()
    {
        while (n_) bit(1);
    }
    void align_zeros()
    {
        while (n_) bit(0);
    }
    void trailing()
    {
        bit(1);
        align_zeros();
    }
    void byte(u8 b) { bits(b, 8); }
    usize count() const { return out_.size() * 8 + n_; }
    std::vector<u8> take()
    {
        std::vector<u8> out = std::move(out_);
        out_.clear();
        return out;
    }

private:
    std::vector<u8> out_;
    u8 cur_ = 0;
    u32 n_ = 0;
};

// The bits of `rbsp` from `from` to its stop bit (rbsp_trailing_bits' 1), not included; then the trailing bits again.
void copy_to_stop_bit(BitWriter& w, const std::vector<u8>& rbsp, usize from)
{
    usize last = rbsp.size();
    while (last > 0 && rbsp[last - 1] == 0) last--;
    if (last == 0) {
        w.trailing();
        return;
    }
    u32 trailing = 0;
    while (!((rbsp[last - 1] >> trailing) & 1)) trailing++;
    const usize stop = (last - 1) * 8 + (7 - trailing);
    for (usize i = from; i < stop; i++) w.bit((rbsp[i / 8] >> (7 - i % 8)) & 1);
    w.trailing();
}

// A NAL unit with emulation prevention bytes, after a start code.
void put_nal(std::vector<u8>& out, u8 header, const std::vector<u8>& rbsp)
{
    out.insert(out.end(), {0x00, 0x00, 0x00, 0x01, header});
    u32 zeros = 0;
    for (const u8 b : rbsp) {
        if (zeros >= 2 && b <= 3) {
            out.push_back(0x03);
            zeros = 0;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
}

// ---- the second view's parameter sets

// A subset SPS (H.7.3.2.1.4) as far as the rewriting needs it: the view's SPS (vdec_h264.hpp's reading), where its
// scaling matrices are in the RBSP (copied as they are), and its views' references.
struct SubsetSps {
    Sps sps;
    std::vector<u8> rbsp;
    usize scaling_from = 0, scaling_to = 0; // seq_scaling_matrix_present_flag's bit and what follows it
    // For each view index: its view_id and its inter-view references (view_ids) for anchor and other pictures.
    struct View {
        u32 id = 0;
        std::vector<u32> anchor[2], non_anchor[2];
    };
    std::vector<View> views;
    bool valid = false;
};

std::optional<SubsetSps> read_subset_sps(const Nal& n)
{
    SubsetSps out;
    Rbsp b = payload_of(n);
    out.rbsp = b.data();
    // seq_parameter_set_data(), noting where the scaling matrices are.
    Sps& s = out.sps;
    s.subset = true;
    s.profile_idc = static_cast<u8>(b.bits(8));
    s.constraints = static_cast<u8>(b.bits(8));
    s.level_idc = static_cast<u8>(b.bits(8));
    s.id = b.ue();
    if (high_profile(s.profile_idc)) {
        s.chroma_format_idc = b.ue();
        if (s.chroma_format_idc == 3) s.separate_colour_plane = b.flag();
        s.bit_depth_luma = b.ue() + 8;
        s.bit_depth_chroma = b.ue() + 8;
        s.transform_bypass = b.flag();
        out.scaling_from = b.position();
        s.scaling_matrix = b.flag();
        if (s.scaling_matrix)
            for (u32 i = 0; i < (s.chroma_format_idc != 3 ? 8u : 12u); i++)
                if (b.flag()) scaling_list(b, i < 6 ? 16 : 64);
        out.scaling_to = b.position();
    }
    s.log2_max_frame_num = b.ue() + 4;
    s.poc_type = b.ue();
    if (s.poc_type == 0) {
        s.log2_max_poc_lsb = b.ue() + 4;
    } else if (s.poc_type == 1) {
        s.delta_pic_order_always_zero = b.flag();
        s.offset_for_non_ref_pic = b.se();
        s.offset_for_top_to_bottom_field = b.se();
        const u32 n_cycle = b.ue();
        for (u32 i = 0; i < n_cycle && i < 256 && !b.over(); i++) s.offset_for_ref_frame.push_back(b.se());
    }
    s.max_num_ref_frames = b.ue();
    s.gaps_allowed = b.flag();
    s.width_mbs = b.ue() + 1;
    s.height_map_units = b.ue() + 1;
    s.frame_mbs_only = b.flag();
    if (!s.frame_mbs_only) s.mbaff = b.flag();
    s.direct_8x8 = b.flag();
    s.cropping = b.flag();
    if (s.cropping) {
        s.crop_left = b.ue();
        s.crop_right = b.ue();
        s.crop_top = b.ue();
        s.crop_bottom = b.ue();
    }
    s.vui_present = b.flag();
    if (s.vui_present) s.vui = parse_vui(b);
    if (s.profile_idc != 118 && s.profile_idc != 128 && s.profile_idc != 134) return std::nullopt;
    if (!b.bit()) return std::nullopt; // bit_equal_to_one
    // seq_parameter_set_mvc_extension() (H.7.3.2.1.4)
    const u32 num_views = b.ue() + 1;
    if (num_views > 1024) return std::nullopt;
    out.views.resize(num_views);
    for (u32 i = 0; i < num_views && !b.over(); i++) out.views[i].id = b.ue();
    for (u32 i = 1; i < num_views && !b.over(); i++)
        for (u32 list = 0; list < 2; list++) {
            const u32 n_refs = b.ue();
            for (u32 j = 0; j < n_refs && j < 16 && !b.over(); j++) out.views[i].anchor[list].push_back(b.ue());
        }
    for (u32 i = 1; i < num_views && !b.over(); i++)
        for (u32 list = 0; list < 2; list++) {
            const u32 n_refs = b.ue();
            for (u32 j = 0; j < n_refs && j < 16 && !b.over(); j++) out.views[i].non_anchor[list].push_back(b.ue());
        }
    s.num_views = num_views;
    for (const auto& v : out.views) s.view_ids.push_back(v.id);
    out.valid = !b.over() && s.log2_max_frame_num <= 16 && s.poc_type <= 2 && s.log2_max_poc_lsb <= 16;
    s.valid = out.valid;
    return out;
}

// A PPS's RBSP with another id and SPS id, its bottom_field_pic_order_in_frame_present_flag flipped when `flip` (the
// rest as it was).
std::vector<u8> renumber_pps(const std::vector<u8>& rbsp, u32 id, u32 sps_id, bool flip)
{
    Rbsp b(rbsp);
    b.ue();
    b.ue();
    const u32 cabac = b.bit();
    const u32 bottom = b.bit();
    BitWriter w;
    w.ue(id);
    w.ue(sps_id);
    w.bit(cabac);
    w.bit(bottom ^ (flip ? 1u : 0u));
    copy_to_stop_bit(w, rbsp, b.position());
    return w.take();
}

// The made stream's PPS ids for a second view PPS (the k-th seen): a CAVLC slice's data keeps its bit alignment (an
// I_PCM macroblock's samples are byte-aligned) only when the made header's length matches the original's modulo 8,
// so each PPS has eight copies: ids of 5, 7, 9 and 11 bits, each with bottom_field_pic_order_in_frame_present_flag as
// it was and flipped (a slice header's delta_pic_order_cnt_bottom, one bit for 0, there or not).
constexpr std::array<u32, 4> kVariantIds{3, 7, 15, 31};
u32 made_pps_id(u32 k, u32 variant) { return kVariantIds[variant >> 1] + 2 * k + (variant & 1); }

// ---- the second view's slices

struct RefOp {
    u32 idc = 0, value = 0;
};

struct Weights {
    bool luma = false, chroma = false;
    s32 luma_weight = 0, luma_offset = 0;
    s32 chroma_weight[2] = {0, 0}, chroma_offset[2] = {0, 0};
};

// A slice header (7.3.3 with H.7.3.3.1.1's list modification) read whole, for writing again; and where its data starts.
struct SliceSyntax {
    u32 first_mb = 0, slice_type = 0, pps_id = 0;
    u32 frame_num = 0, idr_pic_id = 0, poc_lsb = 0;
    s32 delta_poc_bottom = 0, delta_poc[2] = {0, 0};
    u32 redundant_pic_cnt = 0;
    bool direct_spatial = false;
    u32 num_ref_idx[2] = {0, 0};
    bool modification[2] = {false, false};
    std::vector<RefOp> ops[2];
    bool weighted = false;
    u32 luma_log2 = 0, chroma_log2 = 0;
    std::vector<Weights> weights[2];
    bool no_output_of_prior_pics = false, long_term_reference = false, adaptive = false;
    std::vector<Mmco> mmco;
    u32 cabac_init_idc = 0;
    s32 qp_delta = 0;
    bool sp_for_switch = false;
    s32 qs_delta = 0;
    u32 deblock_idc = 0;
    s32 alpha = 0, beta = 0;
    usize data_bit = 0;
    bool valid = false;

    u32 type() const { return slice_type % 5; }
    bool is_b() const { return type() == SLICE_B; }
    bool is_p() const { return type() == SLICE_P || type() == SLICE_SP; }
    bool is_i() const { return type() == SLICE_I || type() == SLICE_SI; }
};

SliceSyntax read_slice(Rbsp& b, bool idr, u32 ref_idc, const Sps& s, const Pps& p)
{
    SliceSyntax h;
    h.first_mb = b.ue();
    h.slice_type = b.ue();
    h.pps_id = b.ue();
    if (h.slice_type > 9) return h;
    h.frame_num = b.bits(s.log2_max_frame_num);
    if (idr) h.idr_pic_id = b.ue();
    if (s.poc_type == 0) {
        h.poc_lsb = b.bits(s.log2_max_poc_lsb);
        if (p.bottom_field_pic_order) h.delta_poc_bottom = b.se();
    }
    if (s.poc_type == 1 && !s.delta_pic_order_always_zero) {
        h.delta_poc[0] = b.se();
        if (p.bottom_field_pic_order) h.delta_poc[1] = b.se();
    }
    if (p.redundant_pic_cnt_present) h.redundant_pic_cnt = b.ue();
    if (h.is_b()) h.direct_spatial = b.flag();
    h.num_ref_idx[0] = p.num_ref_idx_l0_default;
    h.num_ref_idx[1] = p.num_ref_idx_l1_default;
    if (h.is_p() || h.is_b()) {
        if (b.flag()) {
            h.num_ref_idx[0] = b.ue() + 1;
            if (h.is_b()) h.num_ref_idx[1] = b.ue() + 1;
        }
    }
    if (h.num_ref_idx[0] > 32 || h.num_ref_idx[1] > 32) return h;
    for (u32 list = 0; list < (h.is_b() ? 2u : h.is_i() ? 0u : 1u); list++) {
        h.modification[list] = b.flag();
        if (!h.modification[list]) continue;
        for (u32 k = 0; k < 100 && !b.over(); k++) {
            RefOp op;
            op.idc = b.ue();
            if (op.idc == 3) break;
            op.value = b.ue();
            h.ops[list].push_back(op);
        }
    }
    if ((p.weighted_pred && h.is_p()) || (p.weighted_bipred_idc == 1 && h.is_b())) {
        h.weighted = true;
        h.luma_log2 = b.ue();
        if (s.chroma_format_idc != 0) h.chroma_log2 = b.ue();
        for (u32 list = 0; list < (h.is_b() ? 2u : 1u); list++) {
            for (u32 i = 0; i < h.num_ref_idx[list] && !b.over(); i++) {
                Weights wt;
                wt.luma = b.flag();
                if (wt.luma) {
                    wt.luma_weight = b.se();
                    wt.luma_offset = b.se();
                }
                if (s.chroma_format_idc != 0) {
                    wt.chroma = b.flag();
                    if (wt.chroma)
                        for (u32 j = 0; j < 2; j++) {
                            wt.chroma_weight[j] = b.se();
                            wt.chroma_offset[j] = b.se();
                        }
                }
                h.weights[list].push_back(wt);
            }
        }
    }
    if (ref_idc) {
        if (idr) {
            h.no_output_of_prior_pics = b.flag();
            h.long_term_reference = b.flag();
        } else {
            h.adaptive = b.flag();
            if (h.adaptive)
                for (u32 k = 0; k < 66 && !b.over(); k++) {
                    Mmco m;
                    m.op = b.ue();
                    if (m.op == 0) break;
                    if (m.op == 1 || m.op == 3) m.difference_of_pic_nums_minus1 = b.ue();
                    if (m.op == 2) m.long_term_pic_num = b.ue();
                    if (m.op == 3 || m.op == 6) m.long_term_frame_idx = b.ue();
                    if (m.op == 4) m.max_long_term_frame_idx_plus1 = b.ue();
                    h.mmco.push_back(m);
                }
        }
    }
    if (p.cabac && !h.is_i()) h.cabac_init_idc = b.ue();
    h.qp_delta = b.se();
    if (h.type() == SLICE_SP || h.type() == SLICE_SI) {
        if (h.type() == SLICE_SP) h.sp_for_switch = b.flag();
        h.qs_delta = b.se();
    }
    if (p.deblocking_control) {
        h.deblock_idc = b.ue();
        if (h.deblock_idc != 1) {
            h.alpha = b.se();
            h.beta = b.se();
        }
    }
    h.data_bit = b.position();
    h.valid = !b.over() && h.first_mb < s.width_mbs * s.frame_height_mbs();
    return h;
}

// ---- the second view's pictures, and the made decoder's

// A picture of the made stream: a base view picture made an I picture (one per access unit) or a second view picture.
struct Pic {
    bool base = false;
    u32 frame_num = 0;   // the second view's frame_num (a second view picture)
    s32 poc = 0;
    u32 made_num = 0;    // its frame_num in the made stream
    bool reference = false;
};

} // namespace

struct SecondView::Impl {
#if PSPOCKET_HAVE_FFMPEG
    AVCodecContext* av = nullptr;
    AVFrame* captured = nullptr;
    AVFrame* scratch = nullptr;
    bool got = false;
#endif
    std::array<std::optional<SubsetSps>, 32> subset{};
    std::array<std::optional<std::vector<u8>>, 256> pps_rbsp{};
    std::array<std::optional<Pps>, 256> pps{};
    // the made stream
    bool configured = false;
    u32 sps_key = ~0u; // the subset SPS id the made SPS was written for
    std::vector<u8> pending;      // parameter sets to give with the next picture
    std::array<bool, 256> pps_sent{};   // the made stream's PPS ids given to its decoder
    std::vector<u32> pps_order;         // the second view's PPS ids in the order seen (their made ids' k)
    u32 prev_ref_made = 0;        // PrevRefFrameNum of the made stream
    u32 idr_pic_id = 0;
    std::vector<Pic> refs;        // the made decoder's references (the second view's and the last base picture)
    std::optional<Pic> last_base; // the base picture of the access unit before, still a reference
    PocState poc_state;
    bool reported = false;

    ~Impl() { close(); }

    void close()
    {
#if PSPOCKET_HAVE_FFMPEG
        if (captured) av_frame_free(&captured);
        if (scratch) av_frame_free(&scratch);
        avcodec_free_context(&av);
#endif
        configured = false;
    }

    void reset()
    {
        close();
        pending.clear();
        pps_sent.fill(false);
        prev_ref_made = 0;
        refs.clear();
        last_base.reset();
        poc_state = {};
        sps_key = ~0u;
    }

#if PSPOCKET_HAVE_FFMPEG
    static int get_buffer(AVCodecContext* c, AVFrame* f, int flags)
    {
        const int r = avcodec_default_get_buffer2(c, f, flags);
        auto* self = static_cast<Impl*>(c->opaque);
        if (r >= 0 && self && self->captured) {
            av_frame_unref(self->captured);
            if (av_frame_ref(self->captured, f) >= 0) self->got = true;
        }
        return r;
    }

    bool open()
    {
        close();
        const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!dec) return false;
        av = avcodec_alloc_context3(dec);
        av->thread_count = 1;
        av->apply_cropping = 0;
        av->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
        av->opaque = this;
        av->get_buffer2 = get_buffer;
        if (avcodec_open2(av, dec, nullptr) < 0) {
            avcodec_free_context(&av);
            return false;
        }
        captured = av_frame_alloc();
        scratch = av_frame_alloc();
        configured = true;
        return true;
    }

    bool feed(const std::vector<u8>& data)
    {
        got = false;
        std::vector<u8> copy(data.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        std::memcpy(copy.data(), data.data(), data.size());
        AVPacket* pkt = av_packet_alloc();
        pkt->data = copy.data();
        pkt->size = static_cast<int>(data.size());
        const int r = avcodec_send_packet(av, pkt);
        av_packet_free(&pkt);
        while (avcodec_receive_frame(av, scratch) == 0) av_frame_unref(scratch);
        return r >= 0 && got;
    }
#endif

    // PicNum in the made stream (frame_num wrap at 2^16, as its SPS says).
    s32 made_pic_num(u32 made_num, u32 current) const
    {
        return made_num > current ? static_cast<s32>(made_num) - 65536 : static_cast<s32>(made_num);
    }

    // The made stream's SPS for the second view's subset SPS: its samples' format and size, a frame_num and a POC of
    // 16 bits, room for the second view's references and two base pictures, no VUI.
    std::vector<u8> made_sps(const SubsetSps& ss) const
    {
        const Sps& s = ss.sps;
        BitWriter w;
        w.byte(100);
        w.byte(0);
        w.byte(51);
        w.ue(0);
        w.ue(s.chroma_format_idc);
        if (s.chroma_format_idc == 3) w.bit(s.separate_colour_plane ? 1 : 0);
        w.ue(s.bit_depth_luma - 8);
        w.ue(s.bit_depth_chroma - 8);
        w.bit(s.transform_bypass ? 1 : 0);
        for (usize i = ss.scaling_from; i < ss.scaling_to; i++) w.bit((ss.rbsp[i / 8] >> (7 - i % 8)) & 1);
        w.ue(16 - 4); // log2_max_frame_num_minus4
        w.ue(0);      // pic_order_cnt_type
        w.ue(16 - 4); // log2_max_pic_order_cnt_lsb_minus4
        w.ue(std::min<u32>(16, std::max<u32>(s.max_num_ref_frames, 1) + 2));
        w.bit(0); // gaps_in_frame_num_value_allowed_flag
        w.ue(s.width_mbs - 1);
        w.ue(s.height_map_units - 1);
        w.bit(1); // frame_mbs_only_flag
        w.bit(s.direct_8x8 ? 1 : 0);
        w.bit(0); // frame_cropping_flag
        w.bit(0); // vui_parameters_present_flag
        w.trailing();
        return w.take();
    }

    // The made stream's PPS for the base view's I pictures (id 0): CAVLC, deblocking off in the slice header.
    static std::vector<u8> base_pps()
    {
        BitWriter w;
        w.ue(0);  // pic_parameter_set_id
        w.ue(0);  // seq_parameter_set_id
        w.bit(0); // entropy_coding_mode_flag
        w.bit(0); // bottom_field_pic_order_in_frame_present_flag
        w.ue(0);  // num_slice_groups_minus1
        w.ue(0);
        w.ue(0);      // num_ref_idx_l0/l1_default_active_minus1
        w.bit(0);     // weighted_pred_flag
        w.bits(0, 2); // weighted_bipred_idc
        w.se(0);
        w.se(0);
        w.se(0);  // pic_init_qp, pic_init_qs, chroma_qp_index_offset
        w.bit(1); // deblocking_filter_control_present_flag
        w.bit(0); // constrained_intra_pred_flag
        w.bit(0); // redundant_pic_cnt_present_flag
        w.trailing();
        return w.take();
    }

    // The base view's picture as an I picture of I_PCM macroblocks (an IDR one when `idr`), removing the base picture
    // before it from the references.
    std::vector<u8> base_picture(const BaseView& base, bool idr, u32 made_num, u32 poc_lsb, const std::optional<Pic>& drop)
    {
        BitWriter w;
        const u32 mbw = base.width / 16, mbh = base.height / 16;
        w.ue(0); // first_mb_in_slice
        w.ue(7); // slice_type: I (all slices)
        w.ue(0); // pic_parameter_set_id
        w.bits(made_num, 16);
        if (idr) w.ue(idr_pic_id);
        w.bits(poc_lsb & 0xFFFF, 16);
        if (idr) {
            w.bit(0); // no_output_of_prior_pics_flag
            w.bit(0); // long_term_reference_flag
        } else {
            w.bit(1); // adaptive_ref_pic_marking_mode_flag
            if (drop) {
                w.ue(1);
                w.ue(static_cast<u32>(made_pic_num(made_num, made_num) - made_pic_num(drop->made_num, made_num) - 1));
            }
            w.ue(0);
        }
        w.se(0);  // slice_qp_delta
        w.ue(1);  // disable_deblocking_filter_idc
        for (u32 my = 0; my < mbh; my++)
            for (u32 mx = 0; mx < mbw; mx++) {
                w.ue(25); // mb_type I_PCM
                w.align_zeros();
                for (u32 y = 0; y < 16; y++)
                    for (u32 x = 0; x < 16; x++) w.byte(base.y[(my * 16 + y) * base.width + mx * 16 + x]);
                for (const std::vector<u8>* plane : {&base.u, &base.v})
                    for (u32 y = 0; y < 8; y++)
                        for (u32 x = 0; x < 8; x++) w.byte((*plane)[(my * 8 + y) * (base.width / 2) + mx * 8 + x]);
            }
        w.trailing();
        std::vector<u8> out;
        put_nal(out, static_cast<u8>(0x60 | (idr ? NAL_IDR : NAL_SLICE)), w.take());
        return out;
    }
};

SecondView::SecondView() : impl_(std::make_unique<Impl>()) {}
SecondView::~SecondView() = default;

void SecondView::reset() { impl_->reset(); }

bool SecondView::decode(std::span<const u8> au, const BaseView& base, ViewSamples& out)
{
#if !PSPOCKET_HAVE_FFMPEG
    (void)au;
    (void)base;
    (void)out;
    return false;
#else
    Impl& m = *impl_;
    const auto fail = [&](std::string_view why) {
        if (!m.reported) PS_LOG_WARN(kChan, "the second view is not decoded ({}): the base view's samples stand for it", why);
        m.reported = true;
        return false;
    };
    const std::vector<Nal> nals = split_nals(au);
    // The second view's parameter sets, then its slices (view 1's coded slice extensions).
    std::vector<const Nal*> slices;
    for (const Nal& n : nals) {
        if (n.type == NAL_SUBSET_SPS) {
            std::optional<SubsetSps> ss = read_subset_sps(n);
            if (ss && ss->valid && ss->sps.id < 32) m.subset[ss->sps.id] = std::move(*ss);
        } else if (n.type == NAL_PPS) {
            Rbsp b = payload_of(n);
            const Pps p = parse_pps(b);
            if (p.valid && p.id < 256) {
                m.pps[p.id] = p;
                m.pps_rbsp[p.id] = b.data();
                for (u32 v = 0; v < 8; v++)
                    if (const auto it = std::find(m.pps_order.begin(), m.pps_order.end(), p.id); it != m.pps_order.end())
                        m.pps_sent[made_pps_id(static_cast<u32>(it - m.pps_order.begin()), v)] = false;
            }
        } else if (n.type == NAL_SLICE_EXT && n.view_id != 0) {
            slices.push_back(&n);
        }
    }
    if (slices.empty()) return false;
    const Nal& first_nal = *slices.front();
    const bool idr = !first_nal.non_idr;
    const u32 ref_idc = first_nal.ref_idc;
    // The first slice's parameter sets.
    Rbsp probe = payload_of(first_nal);
    probe.ue();
    probe.ue();
    const u32 pps_id = probe.ue();
    if (pps_id > 254 || !m.pps[pps_id] || !m.pps[pps_id]->valid) return fail("no PPS");
    const Pps& pps = *m.pps[pps_id];
    if (pps.sps_id > 31 || !m.subset[pps.sps_id]) return fail("no subset SPS");
    const SubsetSps& ss = *m.subset[pps.sps_id];
    const Sps& sps = ss.sps;
    if (!sps.frame_mbs_only) return fail("field pictures");
    if (sps.chroma_format_idc != 1 || sps.bit_depth_luma != 8 || sps.bit_depth_chroma != 8) return fail("not 4:2:0 8-bit");
    if (sps.width() != base.width || sps.height() != base.height) return fail("the views' sizes differ");
    if (base.y.size() < base.width * base.height) return fail("no base picture");
    // The view's references in its subset SPS (by view index).
    const auto view_index = [&](u32 view_id) -> int {
        for (usize i = 0; i < ss.views.size(); i++)
            if (ss.views[i].id == view_id) return static_cast<int>(i);
        return -1;
    };
    const int vi = view_index(first_nal.view_id);
    if (vi <= 0) return fail("a view the subset SPS does not have");
    const int base_index = view_index(0) >= 0 ? view_index(0) : 0;
    // Its slices read.
    std::vector<SliceSyntax> parsed;
    std::vector<std::vector<u8>> rbsps;
    for (const Nal* n : slices) {
        Rbsp b = payload_of(*n);
        parsed.push_back(read_slice(b, idr, ref_idc, sps, pps));
        rbsps.push_back(b.data());
        if (!parsed.back().valid) return fail("a slice header it cannot read");
        if (parsed.back().pps_id != pps_id) return fail("slices with other PPSs");
    }
    const SliceSyntax& h0 = parsed.front();
    if (h0.long_term_reference) return fail("long-term references");
    for (const Mmco& mm : h0.mmco)
        if (mm.op != 1) return fail("memory management operations other than 1");
    // The second view's order count.
    SliceHeader sh;
    sh.valid = true;
    sh.slice_type = h0.slice_type;
    sh.frame_num = h0.frame_num;
    sh.poc_lsb = h0.poc_lsb;
    sh.delta_poc_bottom = h0.delta_poc_bottom;
    sh.delta_poc[0] = h0.delta_poc[0];
    sh.delta_poc[1] = h0.delta_poc[1];
    sh.adaptive = h0.adaptive;
    sh.mmco = h0.mmco;
    const Poc poc = picture_order_count(sps, sh, idr, ref_idc, m.poc_state);
    // The made decoder, set up again for a new subset SPS.
    std::vector<u8> made;
    if (!m.configured || m.sps_key != sps.id) {
        if (!m.open()) return fail("FFmpeg's H.264 decoder did not open");
        m.sps_key = sps.id;
        m.pps_sent.fill(false);
        m.refs.clear();
        m.last_base.reset();
        put_nal(made, 0x67, m.made_sps(ss));
        put_nal(made, 0x68, Impl::base_pps());
    }
    // The base picture: an IDR picture with the second view's IDR (or the made stream's first), else an I picture that
    // drops the base picture before it.
    const bool made_idr = idr || (m.refs.empty() && !m.last_base);
    if (made_idr) {
        m.refs.clear();
        m.last_base.reset();
        m.idr_pic_id = (m.idr_pic_id + 1) % 2;
    }
    Pic bp;
    bp.base = true;
    bp.poc = base.poc;
    bp.reference = true;
    bp.made_num = made_idr ? 0 : (m.prev_ref_made + 1) & 0xFFFF;
    const std::optional<Pic> drop = made_idr ? std::nullopt : m.last_base;
    const std::vector<u8> base_nal =
        m.base_picture(base, made_idr, bp.made_num, static_cast<u32>(base.poc), drop);
    made.insert(made.end(), base_nal.begin(), base_nal.end());
    if (!m.feed(made)) return fail("the base view's I picture did not decode");
    if (drop) std::erase_if(m.refs, [&](const Pic& p) { return p.base; });
    m.prev_ref_made = bp.made_num;
    m.last_base = bp;
    m.refs.push_back(bp);
    // The second view's references in its own order (H.8.2.4): its pictures (short-term), then the inter-view one.
    const u32 max_frame_num = sps.max_frame_num();
    const auto pic_num = [&](const Pic& p) -> s32 {
        return p.frame_num > h0.frame_num ? static_cast<s32>(p.frame_num) - static_cast<s32>(max_frame_num)
                                          : static_cast<s32>(p.frame_num);
    };
    // An anchor picture (and an IDR one) is predicted from its access unit's other views only.
    const bool anchor = first_nal.anchor;
    std::vector<const Pic*> own;
    for (const Pic& p : m.refs)
        if (!p.base && p.reference) own.push_back(&p);
    const std::vector<const Pic*> held = own; // the second view's references, for its marking
    if (idr || anchor) own.clear();
    const Pic* inter_view = &m.refs.back(); // the base picture just made
    const SubsetSps::View& view = ss.views[static_cast<usize>(vi)];
    // The new picture of the made stream, and the second view's marking (its sliding window or memory management
    // operations) as the made stream's operations: the same in every slice, done once the picture is decoded.
    const u32 made_num = (m.prev_ref_made + 1) & 0xFFFF;
    std::vector<const Pic*> removed;
    if (ref_idc && !idr) {
        if (h0.adaptive) {
            for (const Mmco& mm : h0.mmco) {
                const s32 num = static_cast<s32>(h0.frame_num) - static_cast<s32>(mm.difference_of_pic_nums_minus1 + 1);
                for (const Pic* p : held)
                    if (pic_num(*p) == num) removed.push_back(p);
            }
        } else if (held.size() >= std::max<u32>(sps.max_num_ref_frames, 1)) {
            const Pic* oldest = nullptr;
            for (const Pic* p : held)
                if (!oldest || pic_num(*p) < pic_num(*oldest)) oldest = p;
            if (oldest) removed.push_back(oldest);
        }
    }
    std::vector<u32> removed_nums;
    for (const Pic* p : removed) removed_nums.push_back(p->made_num);
    std::vector<u8> pictures, pps_nals;
    for (usize k = 0; k < parsed.size(); k++) {
        const SliceSyntax& h = parsed[k];
        std::vector<std::vector<const Pic*>> lists(2);
        for (u32 list = 0; list < 2; list++) {
            std::vector<const Pic*> init;
            if (h.is_p() && list == 0) {
                init = own;
                std::sort(init.begin(), init.end(), [&](const Pic* a, const Pic* b) { return pic_num(*a) > pic_num(*b); });
            } else if (h.is_b()) {
                std::vector<const Pic*> before, after;
                for (const Pic* p : own) (p->poc < poc.frame() ? before : after).push_back(p);
                std::sort(before.begin(), before.end(), [](const Pic* a, const Pic* b) { return a->poc > b->poc; });
                std::sort(after.begin(), after.end(), [](const Pic* a, const Pic* b) { return a->poc < b->poc; });
                if (list == 0) {
                    init = before;
                    init.insert(init.end(), after.begin(), after.end());
                } else {
                    init = after;
                    init.insert(init.end(), before.begin(), before.end());
                }
            } else {
                continue;
            }
            lists[list] = init;
        }
        if (h.is_b() && lists[1].size() > 1 && lists[1] == lists[0]) std::swap(lists[1][0], lists[1][1]);
        std::vector<const Pic*> inter[2];
        for (u32 list = 0; list < 2; list++) {
            const std::vector<u32>& ids = anchor ? view.anchor[list] : view.non_anchor[list];
            for (const u32 id : ids) {
                if (view_index(id) != base_index) return fail("inter-view references other than the base view");
                inter[list].push_back(inter_view);
            }
            if ((list == 0 && (h.is_p() || h.is_b())) || (list == 1 && h.is_b()))
                lists[list].insert(lists[list].end(), inter[list].begin(), inter[list].end());
        }
        // Modified as the slice says (H.8.2.4.3), each list to its number of entries.
        for (u32 list = 0; list < 2; list++) {
            if (!((list == 0 && (h.is_p() || h.is_b())) || (list == 1 && h.is_b()))) {
                lists[list].clear();
                continue;
            }
            std::vector<const Pic*>& l = lists[list];
            const usize n = h.num_ref_idx[list];
            l.resize(std::max(l.size(), n), nullptr);
            s32 pred = static_cast<s32>(h0.frame_num); // CurrPicNum for a frame
            s32 view_pred = -1;
            usize idx = 0;
            for (const RefOp& op : h.ops[list]) {
                const Pic* target = nullptr;
                if (op.idc == 0 || op.idc == 1) {
                    const s32 diff = static_cast<s32>(op.value) + 1;
                    s32 no_wrap = op.idc == 0 ? pred - diff : pred + diff;
                    if (no_wrap < 0) no_wrap += static_cast<s32>(max_frame_num);
                    if (no_wrap >= static_cast<s32>(max_frame_num)) no_wrap -= static_cast<s32>(max_frame_num);
                    pred = no_wrap;
                    const s32 num = no_wrap > static_cast<s32>(h0.frame_num) ? no_wrap - static_cast<s32>(max_frame_num)
                                                                            : no_wrap;
                    for (const Pic* p : own)
                        if (pic_num(*p) == num) target = p;
                } else if (op.idc == 4 || op.idc == 5) {
                    const s32 count = static_cast<s32>(inter[list].size());
                    if (!count) return fail("an inter-view modification with no inter-view reference");
                    const s32 diff = static_cast<s32>(op.value) + 1;
                    s32 v = op.idc == 4 ? view_pred - diff : view_pred + diff;
                    v = ((v % count) + count) % count;
                    view_pred = v;
                    target = inter[list][static_cast<usize>(v)];
                } else {
                    return fail("long-term references");
                }
                if (!target) return fail("a modification naming no reference");
                l.insert(l.begin() + static_cast<std::ptrdiff_t>(idx), target);
                idx++;
                for (usize j = idx; j < l.size(); j++)
                    if (l[j] == target) {
                        l.erase(l.begin() + static_cast<std::ptrdiff_t>(j));
                        break;
                    }
            }
            l.resize(n);
        }
        // The slice written again for the made stream (with the PPS copy that keeps its data's alignment).
        const auto it = std::find(m.pps_order.begin(), m.pps_order.end(), pps_id);
        const u32 kth = static_cast<u32>(it - m.pps_order.begin());
        if (it == m.pps_order.end()) m.pps_order.push_back(pps_id);
        if (kth > 1) return fail("more than two second view PPSs");
        u32 variant = 0;
        BitWriter w;
        const auto header = [&](u32 v) {
            w = BitWriter();
            w.ue(h.first_mb);
            w.ue(h.slice_type);
            w.ue(made_pps_id(kth, v));
            w.bits(made_num, 16);
            w.bits(static_cast<u32>(poc.frame()) & 0xFFFF, 16);
            if (pps.bottom_field_pic_order != ((v & 1) != 0)) w.se(pps.bottom_field_pic_order ? poc.bottom - poc.top : 0);
        if (pps.redundant_pic_cnt_present) w.ue(h.redundant_pic_cnt);
        if (h.is_b()) w.bit(h.direct_spatial ? 1 : 0);
        if (h.is_p() || h.is_b()) {
            w.bit(1); // num_ref_idx_active_override_flag
            w.ue(h.num_ref_idx[0] - 1);
            if (h.is_b()) w.ue(h.num_ref_idx[1] - 1);
        }
        for (u32 list = 0; list < (h.is_b() ? 2u : h.is_i() ? 0u : 1u); list++) {
            w.bit(1); // ref_pic_list_modification_flag (the entries past the last reference are never used)
            s32 made_pred = static_cast<s32>(made_num);
            for (const Pic* p : lists[list]) {
                if (!p) break;
                const s32 num = m.made_pic_num(p->made_num, made_num);
                if (num < made_pred) {
                    w.ue(0);
                    w.ue(static_cast<u32>(made_pred - num - 1));
                } else {
                    w.ue(1);
                    w.ue(static_cast<u32>(num - made_pred - 1));
                }
                made_pred = num;
            }
            w.ue(3);
        }
        if (h.weighted) {
            w.ue(h.luma_log2);
            w.ue(h.chroma_log2);
            for (u32 list = 0; list < (h.is_b() ? 2u : 1u); list++)
                for (const Weights& wt : h.weights[list]) {
                    w.bit(wt.luma ? 1 : 0);
                    if (wt.luma) {
                        w.se(wt.luma_weight);
                        w.se(wt.luma_offset);
                    }
                    w.bit(wt.chroma ? 1 : 0);
                    if (wt.chroma)
                        for (u32 j = 0; j < 2; j++) {
                            w.se(wt.chroma_weight[j]);
                            w.se(wt.chroma_offset[j]);
                        }
                }
        }
        if (ref_idc) {
            w.bit(1); // adaptive_ref_pic_marking_mode_flag
            for (const u32 num : removed_nums) {
                w.ue(1);
                w.ue(static_cast<u32>(static_cast<s32>(made_num) - m.made_pic_num(num, made_num) - 1));
            }
            w.ue(0);
        }
        if (pps.cabac && !h.is_i()) w.ue(h.cabac_init_idc);
        w.se(h.qp_delta);
        if (h.type() == SLICE_SP || h.type() == SLICE_SI) {
            if (h.type() == SLICE_SP) w.bit(h.sp_for_switch ? 1 : 0);
            w.se(h.qs_delta);
        }
        if (pps.deblocking_control) {
            w.ue(h.deblock_idc);
            if (h.deblock_idc != 1) {
                w.se(h.alpha);
                w.se(h.beta);
            }
        }
        return true;
        };
        if (!header(0)) return false;
        if (!pps.cabac) {
            const u32 need = static_cast<u32>((h.data_bit - w.count()) & 7);
            if (pps.bottom_field_pic_order && (need & 1)) return fail("a CAVLC slice whose alignment cannot be kept");
            variant = (need & 1) | ((need >> 1) << 1);
            if (!header(variant)) return false;
        }
        if (!m.pps_sent[made_pps_id(kth, variant)]) {
            put_nal(pps_nals, 0x68, renumber_pps(*m.pps_rbsp[pps_id], made_pps_id(kth, variant), 0, (variant & 1) != 0));
            m.pps_sent[made_pps_id(kth, variant)] = true;
        }
        // The slice data as it was: CABAC's from its byte boundary, CAVLC's bit for bit.
        const std::vector<u8>& rbsp = rbsps[k];
        if (pps.cabac) {
            w.align_ones();
            const usize from = (h.data_bit + 7) / 8 * 8;
            std::vector<u8> bytes = w.take();
            bytes.insert(bytes.end(), rbsp.begin() + static_cast<std::ptrdiff_t>(std::min(rbsp.size(), from / 8)), rbsp.end());
            put_nal(pictures, static_cast<u8>((ref_idc & 3) << 5 | NAL_SLICE), bytes);
        } else {
            copy_to_stop_bit(w, rbsp, h.data_bit);
            put_nal(pictures, static_cast<u8>((ref_idc & 3) << 5 | NAL_SLICE), w.take());
        }
    }
    // the parameter sets the slices need, then the slices
    std::vector<u8> packet = pps_nals;
    packet.insert(packet.end(), pictures.begin(), pictures.end());
    if (!m.feed(packet)) return fail("its slices did not decode");
    for (const u32 num : removed_nums) std::erase_if(m.refs, [&](const Pic& q) { return !q.base && q.made_num == num; });
    if (ref_idc) {
        if (idr) std::erase_if(m.refs, [](const Pic& p) { return !p.base; });
        Pic dp;
        dp.frame_num = h0.frame_num;
        dp.poc = poc.frame();
        dp.made_num = made_num;
        dp.reference = true;
        m.refs.push_back(dp);
        m.prev_ref_made = made_num;
    }
    // Its samples.
    const AVFrame* f = m.captured;
    if (!f || !f->data[0] || f->width < static_cast<int>(base.width) || f->height < static_cast<int>(base.height))
        return fail("no picture");
    const u32 w = base.width, hgt = base.height;
    out.y.resize(w * hgt);
    out.u.resize(w * hgt / 4);
    out.v.resize(w * hgt / 4);
    for (u32 r = 0; r < hgt; r++) std::memcpy(&out.y[r * w], f->data[0] + r * static_cast<u32>(f->linesize[0]), w);
    for (u32 r = 0; r < hgt / 2; r++) {
        std::memcpy(&out.u[r * w / 2], f->data[1] + r * static_cast<u32>(f->linesize[1]), w / 2);
        std::memcpy(&out.v[r * w / 2], f->data[2] + r * static_cast<u32>(f->linesize[2]), w / 2);
    }
    return true;
#endif
}

} // namespace pspocket::hle::vdec::avc
