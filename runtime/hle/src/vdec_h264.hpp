#pragma once

// H.264's high-level syntax (ITU-T H.264 7.3, Annex D and E, H.7.3 for MVC's NAL units), as the AVC and MVC codec
// cores read it (D-933): NAL units, sequence and picture parameter sets with the VUI and HRD, slice headers to the
// reference marking, SEI messages and picture order counts. What Sony's cores report of a stream comes from these
// fields; the samples are FFmpeg's. Written from the standard; nothing here is Sony's code or data.

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::hle::vdec::h264 {

// ---- bits

// An RBSP read bit by bit (exp-Golomb codes); reading past its end gives 0s and marks it.
class Rbsp {
public:
    Rbsp() = default;
    explicit Rbsp(std::vector<u8> d) : d_(std::move(d)) {}
    u32 bit()
    {
        if (pos_ >= d_.size() * 8) {
            over_ = true;
            return 0;
        }
        const u32 b = (d_[pos_ / 8] >> (7 - pos_ % 8)) & 1;
        pos_++;
        return b;
    }
    u32 bits(u32 n)
    {
        u32 v = 0;
        for (u32 i = 0; i < n; i++) v = v << 1 | bit();
        return v;
    }
    bool flag() { return bit() != 0; }
    u32 ue()
    {
        u32 zeros = 0;
        while (!bit()) {
            if (over_ || ++zeros >= 32) {
                over_ = true;
                return 0;
            }
        }
        return (zeros ? (1u << zeros) - 1 + bits(zeros) : 0);
    }
    s32 se()
    {
        const u32 v = ue();
        return (v & 1) ? static_cast<s32>((v + 1) / 2) : -static_cast<s32>(v / 2);
    }
    bool byte_aligned() const { return pos_ % 8 == 0; }
    usize position() const { return pos_; }
    usize size_bits() const { return d_.size() * 8; }
    // 7.2: whether data comes before the rbsp_trailing_bits (a 1 then 0s to the end).
    bool more_rbsp_data() const
    {
        if (pos_ >= d_.size() * 8) return false;
        usize last = d_.size();
        while (last > 0 && d_[last - 1] == 0) last--;
        if (last == 0) return false;
        const u8 b = d_[last - 1];
        u32 trailing = 0;
        while (!((b >> trailing) & 1)) trailing++;
        const usize stop = (last - 1) * 8 + (7 - trailing); // the stop bit's position
        return pos_ < stop;
    }
    bool over() const { return over_; }
    const std::vector<u8>& data() const { return d_; }

private:
    std::vector<u8> d_;
    usize pos_ = 0;
    bool over_ = false;
};

// A NAL unit's payload (after its header bytes) without emulation prevention bytes.
inline std::vector<u8> unescape(std::span<const u8> payload)
{
    std::vector<u8> out;
    out.reserve(payload.size());
    u32 zeros = 0;
    for (const u8 b : payload) {
        if (zeros >= 2 && b == 3) {
            zeros = 0;
            continue;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return out;
}

// ---- NAL units

enum : u8 {
    NAL_SLICE = 1,
    NAL_DPA = 2,
    NAL_DPB = 3,
    NAL_DPC = 4,
    NAL_IDR = 5,
    NAL_SEI = 6,
    NAL_SPS = 7,
    NAL_PPS = 8,
    NAL_AUD = 9,
    NAL_END_SEQ = 10,
    NAL_END_STREAM = 11,
    NAL_FILLER = 12,
    NAL_SPS_EXT = 13,
    NAL_PREFIX = 14,
    NAL_SUBSET_SPS = 15,
    NAL_AUX_SLICE = 19,
    NAL_SLICE_EXT = 20,
};

struct Nal {
    u8 type = 0, ref_idc = 0;
    std::span<const u8> bytes; // from the header byte to the next start code (trailing zeros dropped)
    usize offset = 0;          // the header byte's place in the access unit
    // MVC's three bytes after the header (prefix NAL units and coded slice extensions, H.7.3.1.1).
    bool non_idr = false, anchor = false, inter_view = false;
    u32 view_id = 0, temporal_id = 0, priority_id = 0;
};

// The NAL units of an Annex B byte stream: each from its start code to the next (or the end).
inline std::vector<Nal> split_nals(std::span<const u8> d)
{
    std::vector<Nal> out;
    usize i = 0;
    const auto next = [&](usize from) {
        for (usize k = from; k + 3 <= d.size(); k++)
            if (d[k] == 0 && d[k + 1] == 0 && d[k + 2] == 1) return k;
        return d.size();
    };
    i = next(0);
    while (i < d.size()) {
        const usize begin = i + 3;
        const usize following = next(begin);
        usize end = following;
        while (end > begin && d[end - 1] == 0) end--;
        if (begin < end) {
            Nal n;
            n.bytes = d.subspan(begin, end - begin);
            n.offset = begin;
            n.type = n.bytes[0] & 0x1F;
            n.ref_idc = (n.bytes[0] >> 5) & 3;
            if ((n.type == NAL_PREFIX || n.type == NAL_SLICE_EXT) && n.bytes.size() >= 4 && !(n.bytes[1] & 0x80)) {
                n.non_idr = (n.bytes[1] >> 6) & 1;
                n.priority_id = n.bytes[1] & 0x3F;
                n.view_id = static_cast<u32>(n.bytes[2]) << 2 | n.bytes[3] >> 6;
                n.temporal_id = (n.bytes[3] >> 3) & 7;
                n.anchor = (n.bytes[3] >> 2) & 1;
                n.inter_view = (n.bytes[3] >> 1) & 1;
            }
            out.push_back(n);
        }
        i = following;
    }
    return out;
}

// The payload of a NAL unit as an RBSP (header bytes skipped: three more for MVC's extensions).
inline Rbsp payload_of(const Nal& n)
{
    const usize header = (n.type == NAL_PREFIX || n.type == NAL_SLICE_EXT) ? 4 : 1;
    return Rbsp(unescape(n.bytes.subspan(std::min(header, n.bytes.size()))));
}

// ---- parameter sets

struct Hrd {
    u32 cpb_cnt = 0;
    u8 bit_rate_scale = 0, cpb_size_scale = 0;
    u8 initial_cpb_removal_delay_length = 24, cpb_removal_delay_length = 24, dpb_output_delay_length = 24,
       time_offset_length = 24;
};

struct Vui {
    bool aspect_ratio_info = false;
    u8 aspect_ratio_idc = 0;
    u16 sar_width = 0, sar_height = 0;
    bool overscan_info = false, overscan_appropriate = false;
    bool video_signal_type = false;
    u8 video_format = 5;
    bool full_range = false, colour_description = false;
    u8 primaries = 2, transfer = 2, matrix = 2;
    bool chroma_loc_info = false;
    u32 chroma_top = 0, chroma_bottom = 0;
    bool timing_info = false;
    u32 units_in_tick = 0, time_scale = 0;
    bool fixed_frame_rate = false;
    bool nal_hrd = false, vcl_hrd = false;
    Hrd nal, vcl;
    bool low_delay_hrd = false, pic_struct_present = false;
    bool bitstream_restriction = false, mv_over_boundaries = true;
    u32 max_bytes_per_pic_denom = 2, max_bits_per_mb_denom = 1, log2_max_mv_length_h = 16, log2_max_mv_length_v = 16;
    u32 num_reorder_frames = 16, max_dec_frame_buffering = 16;
};

struct Sps {
    bool valid = false;
    u8 profile_idc = 0, constraints = 0, level_idc = 0;
    u32 id = 0;
    u32 chroma_format_idc = 1;
    bool separate_colour_plane = false;
    u32 bit_depth_luma = 8, bit_depth_chroma = 8;
    bool transform_bypass = false, scaling_matrix = false;
    u32 log2_max_frame_num = 4;
    u32 poc_type = 0, log2_max_poc_lsb = 4;
    bool delta_pic_order_always_zero = false;
    s32 offset_for_non_ref_pic = 0, offset_for_top_to_bottom_field = 0;
    std::vector<s32> offset_for_ref_frame;
    u32 max_num_ref_frames = 0;
    bool gaps_allowed = false;
    u32 width_mbs = 0, height_map_units = 0;
    bool frame_mbs_only = true, mbaff = false, direct_8x8 = false;
    bool cropping = false;
    u32 crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    bool vui_present = false;
    Vui vui;
    // MVC's subset SPS (H.7.3.2.1.4): the views.
    bool subset = false;
    u32 num_views = 1;
    std::vector<u32> view_ids;

    u32 frame_height_mbs() const { return height_map_units * (frame_mbs_only ? 1 : 2); }
    u32 width() const { return width_mbs * 16; }
    u32 height() const { return frame_height_mbs() * 16; }
    u32 max_frame_num() const { return 1u << log2_max_frame_num; }
    // The cropping in samples (7.4.2.1.1): CropUnitX and CropUnitY.
    u32 crop_unit_x() const { return chroma_format_idc == 0 || separate_colour_plane || chroma_format_idc == 3 ? 1 : 2; }
    u32 crop_unit_y() const
    {
        const u32 sub = chroma_format_idc == 1 && !separate_colour_plane ? 2 : 1;
        return sub * (frame_mbs_only ? 1 : 2);
    }
};

inline void scaling_list(Rbsp& b, u32 size)
{
    s32 last = 8, next = 8;
    for (u32 j = 0; j < size; j++) {
        if (next) next = (last + b.se() + 256) % 256;
        last = next ? next : last;
    }
}

inline Hrd parse_hrd(Rbsp& b)
{
    Hrd h;
    h.cpb_cnt = b.ue() + 1;
    h.bit_rate_scale = static_cast<u8>(b.bits(4));
    h.cpb_size_scale = static_cast<u8>(b.bits(4));
    for (u32 i = 0; i < h.cpb_cnt && i < 32 && !b.over(); i++) {
        b.ue();
        b.ue();
        b.bit();
    }
    h.initial_cpb_removal_delay_length = static_cast<u8>(b.bits(5) + 1);
    h.cpb_removal_delay_length = static_cast<u8>(b.bits(5) + 1);
    h.dpb_output_delay_length = static_cast<u8>(b.bits(5) + 1);
    h.time_offset_length = static_cast<u8>(b.bits(5));
    return h;
}

inline Vui parse_vui(Rbsp& b)
{
    Vui v;
    v.aspect_ratio_info = b.flag();
    if (v.aspect_ratio_info) {
        v.aspect_ratio_idc = static_cast<u8>(b.bits(8));
        if (v.aspect_ratio_idc == 255) {
            v.sar_width = static_cast<u16>(b.bits(16));
            v.sar_height = static_cast<u16>(b.bits(16));
        }
    }
    v.overscan_info = b.flag();
    if (v.overscan_info) v.overscan_appropriate = b.flag();
    v.video_signal_type = b.flag();
    if (v.video_signal_type) {
        v.video_format = static_cast<u8>(b.bits(3));
        v.full_range = b.flag();
        v.colour_description = b.flag();
        if (v.colour_description) {
            v.primaries = static_cast<u8>(b.bits(8));
            v.transfer = static_cast<u8>(b.bits(8));
            v.matrix = static_cast<u8>(b.bits(8));
        }
    }
    v.chroma_loc_info = b.flag();
    if (v.chroma_loc_info) {
        v.chroma_top = b.ue();
        v.chroma_bottom = b.ue();
    }
    v.timing_info = b.flag();
    if (v.timing_info) {
        v.units_in_tick = b.bits(32);
        v.time_scale = b.bits(32);
        v.fixed_frame_rate = b.flag();
    }
    v.nal_hrd = b.flag();
    if (v.nal_hrd) v.nal = parse_hrd(b);
    v.vcl_hrd = b.flag();
    if (v.vcl_hrd) v.vcl = parse_hrd(b);
    if (v.nal_hrd || v.vcl_hrd) v.low_delay_hrd = b.flag();
    v.pic_struct_present = b.flag();
    v.bitstream_restriction = b.flag();
    if (v.bitstream_restriction) {
        v.mv_over_boundaries = b.flag();
        v.max_bytes_per_pic_denom = b.ue();
        v.max_bits_per_mb_denom = b.ue();
        v.log2_max_mv_length_h = b.ue();
        v.log2_max_mv_length_v = b.ue();
        v.num_reorder_frames = b.ue();
        v.max_dec_frame_buffering = b.ue();
    }
    return v;
}

inline bool high_profile(u32 profile)
{
    return profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
           profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
           profile == 135;
}

// seq_parameter_set_data() (7.3.2.1.1), then for a subset SPS of MVC's profiles its extension's views.
inline Sps parse_sps(Rbsp& b, bool subset = false)
{
    Sps s;
    s.subset = subset;
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
        s.scaling_matrix = b.flag();
        if (s.scaling_matrix)
            for (u32 i = 0; i < (s.chroma_format_idc != 3 ? 8u : 12u); i++)
                if (b.flag()) scaling_list(b, i < 6 ? 16 : 64);
    }
    s.log2_max_frame_num = b.ue() + 4;
    s.poc_type = b.ue();
    if (s.poc_type == 0) {
        s.log2_max_poc_lsb = b.ue() + 4;
    } else if (s.poc_type == 1) {
        s.delta_pic_order_always_zero = b.flag();
        s.offset_for_non_ref_pic = b.se();
        s.offset_for_top_to_bottom_field = b.se();
        const u32 n = b.ue();
        for (u32 i = 0; i < n && i < 256 && !b.over(); i++) s.offset_for_ref_frame.push_back(b.se());
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
    if (subset && (s.profile_idc == 118 || s.profile_idc == 128 || s.profile_idc == 134)) {
        b.bit(); // bit_equal_to_one
        s.num_views = b.ue() + 1;
        for (u32 i = 0; i < s.num_views && i < 1024 && !b.over(); i++) s.view_ids.push_back(b.ue());
    }
    s.valid = !b.over() && s.log2_max_frame_num <= 16 && s.poc_type <= 2 && s.log2_max_poc_lsb <= 16;
    return s;
}

struct Pps {
    bool valid = false;
    u32 id = 0, sps_id = 0;
    bool cabac = false, bottom_field_pic_order = false;
    u32 num_slice_groups = 1;
    u32 num_ref_idx_l0_default = 1, num_ref_idx_l1_default = 1;
    bool weighted_pred = false;
    u32 weighted_bipred_idc = 0;
    s32 pic_init_qp = 26, pic_init_qs = 26, chroma_qp_index_offset = 0;
    bool deblocking_control = false, constrained_intra_pred = false, redundant_pic_cnt_present = false;
    bool transform_8x8 = false;
};

// pic_parameter_set_rbsp() (7.3.2.2); slice groups are read past (FMO is Baseline's and Extended's).
inline Pps parse_pps(Rbsp& b, u32 pic_size_in_map_units = 0)
{
    Pps p;
    p.id = b.ue();
    p.sps_id = b.ue();
    p.cabac = b.flag();
    p.bottom_field_pic_order = b.flag();
    p.num_slice_groups = b.ue() + 1;
    if (p.num_slice_groups > 1) {
        const u32 map_type = b.ue();
        if (map_type == 0) {
            for (u32 i = 0; i < p.num_slice_groups && !b.over(); i++) b.ue();
        } else if (map_type == 2) {
            for (u32 i = 0; i + 1 < p.num_slice_groups && !b.over(); i++) {
                b.ue();
                b.ue();
            }
        } else if (map_type >= 3 && map_type <= 5) {
            b.bit();
            b.ue();
        } else if (map_type == 6) {
            const u32 n = b.ue() + 1;
            u32 bits = 0;
            while ((1u << bits) < p.num_slice_groups) bits++;
            for (u32 i = 0; i < n && !b.over(); i++) b.bits(bits);
            (void)pic_size_in_map_units;
        }
    }
    p.num_ref_idx_l0_default = b.ue() + 1;
    p.num_ref_idx_l1_default = b.ue() + 1;
    p.weighted_pred = b.flag();
    p.weighted_bipred_idc = b.bits(2);
    p.pic_init_qp = 26 + b.se();
    p.pic_init_qs = 26 + b.se();
    p.chroma_qp_index_offset = b.se();
    p.deblocking_control = b.flag();
    p.constrained_intra_pred = b.flag();
    p.redundant_pic_cnt_present = b.flag();
    if (b.more_rbsp_data()) {
        p.transform_8x8 = b.flag();
        if (b.flag())
            for (u32 i = 0; i < 6u + (p.transform_8x8 ? 2u : 0u); i++)
                if (b.flag()) scaling_list(b, i < 6 ? 16 : 64);
        b.se(); // second_chroma_qp_index_offset
    }
    p.valid = !b.over() && p.num_ref_idx_l0_default <= 32 && p.num_ref_idx_l1_default <= 32;
    return p;
}

// ---- slice headers

enum : u32 { SLICE_P = 0, SLICE_B = 1, SLICE_I = 2, SLICE_SP = 3, SLICE_SI = 4 };

struct Mmco {
    u32 op = 0, difference_of_pic_nums_minus1 = 0, long_term_pic_num = 0, long_term_frame_idx = 0,
        max_long_term_frame_idx_plus1 = 0;
};

struct SliceHeader {
    bool valid = false;
    u32 first_mb = 0, slice_type = 0, pps_id = 0, colour_plane = 0;
    u32 frame_num = 0;
    bool field_pic = false, bottom_field = false;
    u32 idr_pic_id = 0;
    u32 poc_lsb = 0;
    s32 delta_poc_bottom = 0;
    s32 delta_poc[2] = {0, 0};
    u32 redundant_pic_cnt = 0;
    bool direct_spatial = false;
    u32 num_ref_idx_l0 = 0, num_ref_idx_l1 = 0;
    // dec_ref_pic_marking()
    bool no_output_of_prior_pics = false, long_term_reference = false, adaptive = false;
    std::vector<Mmco> mmco;
    s32 qp_delta = 0;
    u32 disable_deblocking_filter_idc = 0;

    u32 type() const { return slice_type % 5; }
    bool is_b() const { return type() == SLICE_B; }
    bool is_p() const { return type() == SLICE_P || type() == SLICE_SP; }
    bool is_i() const { return type() == SLICE_I || type() == SLICE_SI; }
};

// slice_header() (7.3.3) to slice_qp_delta and the deblocking filter's fields, for the active parameter sets; `idr`
// and `ref_idc` from the NAL unit (for MVC's slice extensions, its non_idr_flag).
inline SliceHeader parse_slice_header(Rbsp& b, bool idr, u32 ref_idc, const Sps* const* spss, const Pps* const* ppss,
    const Sps** active_sps = nullptr, const Pps** active_pps = nullptr)
{
    SliceHeader h;
    h.first_mb = b.ue();
    h.slice_type = b.ue();
    h.pps_id = b.ue();
    if (h.slice_type > 9 || h.pps_id > 255 || !ppss[h.pps_id] || !ppss[h.pps_id]->valid) return h;
    const Pps& p = *ppss[h.pps_id];
    if (p.sps_id > 31 || !spss[p.sps_id] || !spss[p.sps_id]->valid) return h;
    const Sps& s = *spss[p.sps_id];
    if (active_sps) *active_sps = &s;
    if (active_pps) *active_pps = &p;
    if (s.separate_colour_plane) h.colour_plane = b.bits(2);
    h.frame_num = b.bits(s.log2_max_frame_num);
    if (!s.frame_mbs_only) {
        h.field_pic = b.flag();
        if (h.field_pic) h.bottom_field = b.flag();
    }
    if (idr) h.idr_pic_id = b.ue();
    if (s.poc_type == 0) {
        h.poc_lsb = b.bits(s.log2_max_poc_lsb);
        if (p.bottom_field_pic_order && !h.field_pic) h.delta_poc_bottom = b.se();
    }
    if (s.poc_type == 1 && !s.delta_pic_order_always_zero) {
        h.delta_poc[0] = b.se();
        if (p.bottom_field_pic_order && !h.field_pic) h.delta_poc[1] = b.se();
    }
    if (p.redundant_pic_cnt_present) h.redundant_pic_cnt = b.ue();
    if (h.is_b()) h.direct_spatial = b.flag();
    h.num_ref_idx_l0 = p.num_ref_idx_l0_default;
    h.num_ref_idx_l1 = p.num_ref_idx_l1_default;
    if (h.is_p() || h.is_b()) {
        if (b.flag()) { // num_ref_idx_active_override_flag
            h.num_ref_idx_l0 = b.ue() + 1;
            if (h.is_b()) h.num_ref_idx_l1 = b.ue() + 1;
        }
    }
    // ref_pic_list_modification()
    for (u32 list = 0; list < (h.is_b() ? 2u : h.is_i() ? 0u : 1u); list++) {
        if (b.flag()) {
            for (u32 k = 0; k < 100 && !b.over(); k++) {
                const u32 idc = b.ue();
                if (idc == 3) break;
                b.ue();
            }
        }
    }
    // pred_weight_table()
    if ((p.weighted_pred && h.is_p()) || (p.weighted_bipred_idc == 1 && h.is_b())) {
        b.ue(); // luma_log2_weight_denom
        const bool chroma = s.chroma_format_idc != 0;
        if (chroma) b.ue();
        for (u32 list = 0; list < (h.is_b() ? 2u : 1u); list++) {
            const u32 n = list ? h.num_ref_idx_l1 : h.num_ref_idx_l0;
            for (u32 i = 0; i < n && i < 32 && !b.over(); i++) {
                if (b.flag()) {
                    b.se();
                    b.se();
                }
                if (chroma && b.flag())
                    for (u32 j = 0; j < 4; j++) b.se();
            }
        }
    }
    if (ref_idc) {
        if (idr) {
            h.no_output_of_prior_pics = b.flag();
            h.long_term_reference = b.flag();
        } else {
            h.adaptive = b.flag();
            if (h.adaptive) {
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
    }
    if (p.cabac && !h.is_i()) b.ue(); // cabac_init_idc
    h.qp_delta = b.se();
    if (h.type() == SLICE_SP || h.type() == SLICE_SI) {
        if (h.type() == SLICE_SP) b.bit();
        b.se();
    }
    if (p.deblocking_control) {
        h.disable_deblocking_filter_idc = b.ue();
        if (h.disable_deblocking_filter_idc != 1) {
            b.se();
            b.se();
        }
    }
    h.valid = !b.over() && h.first_mb < s.width_mbs * s.frame_height_mbs();
    return h;
}

// ---- SEI

struct SeiMessage {
    u32 type = 0;
    std::vector<u8> payload;
};

// The messages of an SEI RBSP (7.3.2.3).
inline std::vector<SeiMessage> parse_sei(const std::vector<u8>& rbsp)
{
    std::vector<SeiMessage> out;
    usize p = 0;
    while (p < rbsp.size()) {
        if (rbsp[p] == 0x80 && p + 1 >= rbsp.size()) break; // the trailing bits
        u32 type = 0, size = 0;
        while (p < rbsp.size() && rbsp[p] == 0xFF) type += rbsp[p++];
        if (p >= rbsp.size()) break;
        type += rbsp[p++];
        while (p < rbsp.size() && rbsp[p] == 0xFF) size += rbsp[p++];
        if (p >= rbsp.size()) break;
        size += rbsp[p++];
        SeiMessage m;
        m.type = type;
        const usize n = std::min<usize>(size, rbsp.size() - p);
        m.payload.assign(rbsp.begin() + static_cast<std::ptrdiff_t>(p), rbsp.begin() + static_cast<std::ptrdiff_t>(p + n));
        out.push_back(std::move(m));
        p += size;
    }
    return out;
}

// ---- picture order counts (8.2.1)

// What the order count of the previous pictures leaves for the next (8.2.1.1 to 8.2.1.3).
struct PocState {
    s32 prev_poc_msb = 0;
    u32 prev_poc_lsb = 0;
    u32 prev_frame_num = 0;
    s32 prev_frame_num_offset = 0;
    bool prev_mmco5 = false;
    bool prev_ref_bottom_field = false; // the previous reference picture was a bottom field (for MMCO 5)
    s32 prev_ref_top_poc = 0;           // its TopFieldOrderCnt after MMCO 5's adjustment
};

struct Poc {
    s32 top = 0, bottom = 0;
    s32 frame() const { return std::min(top, bottom); }
};

// The order count of a picture (or field) with header `h` of a NAL unit with `ref_idc`, advancing `st`.
inline Poc picture_order_count(const Sps& s, const SliceHeader& h, bool idr, u32 ref_idc, PocState& st)
{
    Poc out;
    const u32 max_frame_num = s.max_frame_num();
    if (s.poc_type == 0) {
        s32 prev_msb = st.prev_poc_msb;
        u32 prev_lsb = st.prev_poc_lsb;
        if (idr) {
            prev_msb = 0;
            prev_lsb = 0;
        } else if (st.prev_mmco5) {
            if (!st.prev_ref_bottom_field) {
                prev_msb = 0;
                prev_lsb = static_cast<u32>(st.prev_ref_top_poc);
            } else {
                prev_msb = 0;
                prev_lsb = 0;
            }
        }
        const s32 max_lsb = 1 << s.log2_max_poc_lsb;
        s32 msb = prev_msb;
        const s32 lsb = static_cast<s32>(h.poc_lsb), plsb = static_cast<s32>(prev_lsb);
        if (lsb < plsb && plsb - lsb >= max_lsb / 2) msb = prev_msb + max_lsb;
        else if (lsb > plsb && lsb - plsb > max_lsb / 2) msb = prev_msb - max_lsb;
        if (!h.field_pic) {
            out.top = msb + lsb;
            out.bottom = out.top + h.delta_poc_bottom;
        } else if (!h.bottom_field) {
            out.top = out.bottom = msb + lsb;
        } else {
            out.top = out.bottom = msb + lsb;
        }
        if (ref_idc) {
            st.prev_poc_msb = msb;
            st.prev_poc_lsb = h.poc_lsb;
        }
    } else {
        s32 frame_num_offset = 0;
        const s32 prev_offset = st.prev_mmco5 ? 0 : st.prev_frame_num_offset;
        if (idr) frame_num_offset = 0;
        else if (st.prev_frame_num > h.frame_num) frame_num_offset = prev_offset + static_cast<s32>(max_frame_num);
        else frame_num_offset = prev_offset;
        if (s.poc_type == 1) {
            const s32 cycle_len = static_cast<s32>(s.offset_for_ref_frame.size());
            s32 abs_frame_num = cycle_len ? frame_num_offset + static_cast<s32>(h.frame_num) : 0;
            if (!ref_idc && abs_frame_num > 0) abs_frame_num--;
            s32 expected = 0;
            if (abs_frame_num > 0) {
                s32 delta_cycle = 0;
                for (const s32 o : s.offset_for_ref_frame) delta_cycle += o;
                const s32 cycle = (abs_frame_num - 1) / cycle_len, in_cycle = (abs_frame_num - 1) % cycle_len;
                expected = cycle * delta_cycle;
                for (s32 i = 0; i <= in_cycle; i++) expected += s.offset_for_ref_frame[static_cast<usize>(i)];
            }
            if (!ref_idc) expected += s.offset_for_non_ref_pic;
            if (!h.field_pic) {
                out.top = expected + h.delta_poc[0];
                out.bottom = out.top + s.offset_for_top_to_bottom_field + h.delta_poc[1];
            } else if (!h.bottom_field) {
                out.top = out.bottom = expected + h.delta_poc[0];
            } else {
                out.top = out.bottom = expected + s.offset_for_top_to_bottom_field + h.delta_poc[0];
            }
        } else {
            s32 temp = 0;
            if (!idr) {
                temp = 2 * (frame_num_offset + static_cast<s32>(h.frame_num));
                if (!ref_idc) temp--;
            }
            out.top = out.bottom = temp;
        }
        st.prev_frame_num_offset = frame_num_offset;
    }
    st.prev_frame_num = h.frame_num;
    return out;
}

} // namespace pspocket::hle::vdec::h264
