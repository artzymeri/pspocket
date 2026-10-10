#pragma once

// A minimal H.264 writer for tests (synthetic video, no game data): Baseline profile, every picture an IDR whose
// macroblocks are all I_PCM, so the samples are stored as they are and any conforming decoder must give them back
// exactly. Annex B byte stream, each access unit an access unit delimiter, the SPS and PPS, then one slice; for MVC,
// a second view's NAL units in the same access unit (D-590).

#include <cstdint>
#include <vector>

namespace pspocket::hle::test::h264 {

class BitWriter {
public:
    void bit(uint32_t b)
    {
        cur_ = static_cast<uint8_t>(cur_ << 1 | (b & 1));
        if (++n_ == 8) flush_byte();
    }
    void bits(uint32_t v, int count)
    {
        for (int i = count - 1; i >= 0; i--) bit(v >> i);
    }
    void ue(uint32_t v)
    {
        const uint64_t x = uint64_t{v} + 1;
        int len = 0;
        while ((x >> len) > 1) len++;
        bits(0, len);
        for (int i = len; i >= 0; i--) bit(static_cast<uint32_t>(x >> i));
    }
    void se(int32_t v) { ue(v > 0 ? static_cast<uint32_t>(2 * v - 1) : static_cast<uint32_t>(-2 * v)); }
    void align_zero()
    {
        while (n_) bit(0);
    }
    void trailing()
    {
        bit(1);
        align_zero();
    }
    [[nodiscard]] bool aligned() const { return n_ == 0; }
    void byte(uint8_t v) { bits(v, 8); }
    std::vector<uint8_t> take()
    {
        std::vector<uint8_t> out;
        out.swap(bytes_);
        return out;
    }

private:
    void flush_byte()
    {
        bytes_.push_back(cur_);
        cur_ = 0;
        n_ = 0;
    }
    std::vector<uint8_t> bytes_;
    uint8_t cur_ = 0;
    int n_ = 0;
};

// A NAL unit with its start code, the payload's emulation prevention bytes inserted.
inline void put_nal(std::vector<uint8_t>& out, uint8_t header, const std::vector<uint8_t>& rbsp)
{
    out.insert(out.end(), {0x00, 0x00, 0x00, 0x01, header});
    int zeros = 0;
    for (const uint8_t b : rbsp) {
        if (zeros >= 2 && b <= 3) {
            out.push_back(0x03);
            zeros = 0;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
}

struct Picture {
    int width = 64, height = 64;           // multiples of 16
    std::vector<uint8_t> y, cb, cr;        // planar 4:2:0, no stride padding
    explicit Picture(int w = 64, int h = 64) : width(w), height(h), y(w * h), cb(w * h / 4), cr(w * h / 4) {}
};

struct Options {
    uint8_t level_idc = 21;
    bool timing = true;                 // VUI timing: 30000/1001 frames per second, fixed
    uint32_t units_in_tick = 1001, time_scale = 60000;
};

// seq_parameter_set_data(): Baseline (66), or Stereo High (128) for MVC's subset SPS, the level, POC type 2 (output
// order is decoding order), one reference frame.
inline void sps_data(BitWriter& w, const Picture& p, const Options& o, uint8_t profile = 66)
{
    w.byte(profile);
    w.byte(profile == 66 ? 0xC0 : 0x00); // constraint_set0 and 1 for Baseline
    w.byte(o.level_idc);
    w.ue(0);      // seq_parameter_set_id
    if (profile != 66) {
        w.ue(1);  // chroma_format_idc: 4:2:0
        w.ue(0);  // bit_depth_luma_minus8
        w.ue(0);  // bit_depth_chroma_minus8
        w.bit(0); // qpprime_y_zero_transform_bypass_flag
        w.bit(0); // seq_scaling_matrix_present_flag
    }
    w.ue(0);      // log2_max_frame_num_minus4
    w.ue(2);      // pic_order_cnt_type
    w.ue(1);      // max_num_ref_frames
    w.bit(0);     // gaps_in_frame_num_value_allowed_flag
    w.ue(static_cast<uint32_t>(p.width / 16 - 1));
    w.ue(static_cast<uint32_t>(p.height / 16 - 1));
    w.bit(1); // frame_mbs_only_flag
    w.bit(1); // direct_8x8_inference_flag
    w.bit(0); // frame_cropping_flag
    w.bit(o.timing ? 1 : 0); // vui_parameters_present_flag
    if (o.timing) {
        w.bit(0); // aspect_ratio_info_present_flag
        w.bit(0); // overscan_info_present_flag
        w.bit(0); // video_signal_type_present_flag
        w.bit(0); // chroma_loc_info_present_flag
        w.bit(1); // timing_info_present_flag
        w.bits(o.units_in_tick, 32);
        w.bits(o.time_scale, 32);
        w.bit(1); // fixed_frame_rate_flag
        w.bit(0); // nal_hrd_parameters_present_flag
        w.bit(0); // vcl_hrd_parameters_present_flag
        w.bit(0); // pic_struct_present_flag
        w.bit(0); // bitstream_restriction_flag
    }
}

// The PPS: CAVLC, one slice group, deblocking control present.
inline std::vector<uint8_t> pps_rbsp()
{
    BitWriter w;
    w.ue(0); // pic_parameter_set_id
    w.ue(0); // seq_parameter_set_id
    w.bit(0); // entropy_coding_mode_flag
    w.bit(0); // bottom_field_pic_order_in_frame_present_flag
    w.ue(0);  // num_slice_groups_minus1
    w.ue(0);  // num_ref_idx_l0_default_active_minus1
    w.ue(0);  // num_ref_idx_l1_default_active_minus1
    w.bit(0); // weighted_pred_flag
    w.bits(0, 2); // weighted_bipred_idc
    w.se(0);  // pic_init_qp_minus26
    w.se(0);  // pic_init_qs_minus26
    w.se(0);  // chroma_qp_index_offset
    w.bit(1); // deblocking_filter_control_present_flag
    w.bit(0); // constrained_intra_pred_flag
    w.bit(0); // redundant_pic_cnt_present_flag
    w.trailing();
    return w.take();
}

// An IDR slice of `p`: every macroblock I_PCM (mb_type 25 in an I slice).
inline std::vector<uint8_t> idr_slice_rbsp(const Picture& p, uint32_t idr_pic_id)
{
    BitWriter w;
    w.ue(0);  // first_mb_in_slice
    w.ue(7);  // slice_type: I, all slices of the picture
    w.ue(0);  // pic_parameter_set_id
    w.bits(0, 4); // frame_num
    w.ue(idr_pic_id);
    w.bit(0); // no_output_of_prior_pics_flag
    w.bit(0); // long_term_reference_flag
    w.se(0);  // slice_qp_delta
    w.ue(1);  // disable_deblocking_filter_idc: off
    const int mbw = p.width / 16, mbh = p.height / 16;
    for (int my = 0; my < mbh; my++) {
        for (int mx = 0; mx < mbw; mx++) {
            w.ue(25);
            w.align_zero(); // pcm_alignment_zero_bit
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) w.byte(p.y[(my * 16 + y) * p.width + mx * 16 + x]);
            for (const std::vector<uint8_t>* plane : {&p.cb, &p.cr})
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) w.byte((*plane)[(my * 8 + y) * (p.width / 2) + mx * 8 + x]);
        }
    }
    w.trailing();
    return w.take();
}

inline std::vector<uint8_t> aud_rbsp()
{
    BitWriter w;
    w.bits(0, 3); // primary_pic_type 0: I slices only
    w.trailing();
    return w.take();
}

inline std::vector<uint8_t> sps_rbsp(const Picture& p, const Options& o)
{
    BitWriter w;
    sps_data(w, p, o);
    w.trailing();
    return w.take();
}

// One access unit: AUD, SPS, PPS, an IDR slice of I_PCM macroblocks.
inline std::vector<uint8_t> access_unit(const Picture& p, uint32_t idr_pic_id, const Options& o = {})
{
    std::vector<uint8_t> out;
    put_nal(out, 0x09, aud_rbsp());
    put_nal(out, 0x67, sps_rbsp(p, o));
    put_nal(out, 0x68, pps_rbsp());
    put_nal(out, 0x65, idr_slice_rbsp(p, idr_pic_id));
    return out;
}

// ---- MVC (H.264 Annex H, D-590): a second view in the same access unit.

// A NAL unit with the three bytes of nal_unit_header_mvc_extension() after its first: svc_extension_flag 0,
// non_idr_flag 0 (an IDR access unit), priority_id 0, view_id, temporal_id 0, anchor_pic_flag 1, inter_view_flag,
// reserved_one_bit. The emulation prevention bytes go into the payload only, as the header is not escaped.
inline void put_mvc_nal(std::vector<uint8_t>& out, uint8_t header, uint32_t view_id, bool inter_view,
    const std::vector<uint8_t>& rbsp)
{
    out.insert(out.end(), {0x00, 0x00, 0x00, 0x01, header, 0x00, static_cast<uint8_t>(view_id >> 2),
        static_cast<uint8_t>((view_id & 3) << 6 | 1 << 2 | (inter_view ? 1 : 0) << 1 | 1)});
    std::vector<uint8_t> nal;
    put_nal(nal, 0, rbsp);
    out.insert(out.end(), nal.begin() + 5, nal.end()); // without put_nal's start code and header byte
}

// The subset SPS (NAL unit 15) of two views: Stereo High, view 1 predicted from view 0, one level for both.
inline std::vector<uint8_t> subset_sps_rbsp(const Picture& p, const Options& o)
{
    BitWriter w;
    sps_data(w, p, o, 128);
    w.bit(1); // bit_equal_to_one
    // seq_parameter_set_mvc_extension()
    w.ue(1); // num_views_minus1
    w.ue(0); // view_id[0]
    w.ue(1); // view_id[1]
    w.ue(1); // num_anchor_refs_l0[1]
    w.ue(0); // anchor_ref_l0[1][0]: view 0
    w.ue(0); // num_anchor_refs_l1[1]
    w.ue(1); // num_non_anchor_refs_l0[1]
    w.ue(0); // non_anchor_ref_l0[1][0]
    w.ue(0); // num_non_anchor_refs_l1[1]
    w.ue(0); // num_level_values_signalled_minus1
    w.byte(o.level_idc);
    w.ue(0);      // num_applicable_ops_minus1[0]
    w.bits(0, 3); // applicable_op_temporal_id
    w.ue(1);      // applicable_op_num_target_views_minus1: both
    w.ue(0);      // applicable_op_target_view_id: 0, 1
    w.ue(1);
    w.ue(1);      // applicable_op_num_views_minus1
    w.bit(0);     // mvc_vui_parameters_present_flag
    w.bit(0);     // additional_extension2_flag
    w.trailing();
    return w.take();
}

// An MVC access unit of two views: the base view's access unit (access_unit()'s NAL units, in its order) with a subset
// SPS after the SPS, a prefix NAL unit (14) before the base view's slice and the second view's slice after it as a
// coded slice extension (20). Without the units 14, 15 and 20 it is access_unit(base, ...) byte for byte.
inline std::vector<uint8_t> mvc_access_unit(const Picture& base, const Picture& second, uint32_t idr_pic_id,
    const Options& o = {})
{
    std::vector<uint8_t> out;
    put_nal(out, 0x09, aud_rbsp());
    put_nal(out, 0x67, sps_rbsp(base, o));
    put_nal(out, 0x6F, subset_sps_rbsp(base, o));
    put_nal(out, 0x68, pps_rbsp());
    put_mvc_nal(out, 0x6E, 0, true, {}); // the prefix NAL unit: no payload in MVC
    put_nal(out, 0x65, idr_slice_rbsp(base, idr_pic_id));
    put_mvc_nal(out, 0x74, 1, false, idr_slice_rbsp(second, idr_pic_id));
    return out;
}

// The second view's slice alone, as an access unit of a stream that sends the views apart: nothing of the base view.
inline std::vector<uint8_t> second_view_only(const Picture& second, uint32_t idr_pic_id)
{
    std::vector<uint8_t> out;
    put_mvc_nal(out, 0x74, 1, false, idr_slice_rbsp(second, idr_pic_id));
    return out;
}

// A picture with a pattern of its own: samples 16..235 (luma) and 16..240 (chroma), different for each `seed`.
inline Picture pattern(int width, int height, int seed)
{
    Picture p(width, height);
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) p.y[y * width + x] = static_cast<uint8_t>(16 + (x * 3 + y * 5 + seed * 29) % 220);
    for (int y = 0; y < height / 2; y++)
        for (int x = 0; x < width / 2; x++) {
            p.cb[y * width / 2 + x] = static_cast<uint8_t>(16 + (x * 7 + seed * 13) % 225);
            p.cr[y * width / 2 + x] = static_cast<uint8_t>(16 + (y * 11 + seed * 17) % 225);
        }
    return p;
}

} // namespace pspocket::hle::test::h264
