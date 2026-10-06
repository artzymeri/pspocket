#pragma once

// A minimal H.264 writer for tests (synthetic video, no game data): Baseline profile, every picture an IDR whose
// macroblocks are all I_PCM, so the samples are stored as they are and any conforming decoder must give them back
// exactly. Annex B byte stream, each access unit an access unit delimiter, the SPS and PPS, then one slice.

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

// One access unit: AUD, SPS, PPS, an IDR slice of I_PCM macroblocks.
inline std::vector<uint8_t> access_unit(const Picture& p, uint32_t idr_pic_id, const Options& o = {})
{
    std::vector<uint8_t> out;
    BitWriter w;
    w.bits(0, 3); // primary_pic_type 0: I slices only
    w.trailing();
    put_nal(out, 0x09, w.take());

    // SPS: Baseline, the level, POC type 2 (output order is decoding order), one reference frame.
    w.byte(66);
    w.byte(0xC0); // constraint_set0 and 1
    w.byte(o.level_idc);
    w.ue(0);      // seq_parameter_set_id
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
    w.trailing();
    put_nal(out, 0x67, w.take());

    // PPS: CAVLC, one slice group, deblocking control present.
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
    put_nal(out, 0x68, w.take());

    // The IDR slice: every macroblock I_PCM (mb_type 25 in an I slice).
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
    put_nal(out, 0x65, w.take());
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
