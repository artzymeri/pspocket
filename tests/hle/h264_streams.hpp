#pragma once

// Synthetic H.264 streams for the codec cores' tests (D-933): more than h264_writer.hpp's IDR pictures, still with
// samples any conforming decoder must give exactly. Pictures are I_PCM macroblocks (their samples stored as they are),
// P pictures all P_Skip (a copy of the picture before them in list 0: every motion vector is 0), B pictures all
// B_Skip (spatial direct with no motion: the rounded average of the first pictures of lists 0 and 1) or I_PCM. With
// these, a stream can have any decoding and output order (picture order count types 0, 1 and 2), reference marking
// (sliding window, memory management operations), frame or field pictures, the VUI's fields and SEI messages, without
// an encoder. Entropy coding is CAVLC (I_PCM and skip runs need nothing else). No game data.

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "h264_writer.hpp"

namespace pspocket::hle::test::h264s {

using h264::BitWriter;
using h264::Picture;
using h264::put_nal;

// ---- the sequence

struct Vui {
    bool present = false;
    int aspect_ratio_idc = -1; // -1: aspect_ratio_info_present_flag 0
    uint16_t sar_width = 0, sar_height = 0;
    int overscan = -1; // -1: not present; else overscan_appropriate_flag
    bool video_signal = false;
    uint8_t video_format = 5, full_range = 0;
    bool colour = false;
    uint8_t primaries = 1, transfer = 1, matrix = 1;
    bool chroma_loc = false;
    uint8_t chroma_top = 0, chroma_bottom = 0;
    bool timing = false;
    uint32_t units_in_tick = 1001, time_scale = 60000;
    bool fixed_rate = true;
    bool nal_hrd = false, vcl_hrd = false; // one CPB each, cpb_cnt_minus1 0
    bool low_delay_hrd = false;
    bool pic_struct = false;
    bool restriction = false;
    uint32_t num_reorder_frames = 0, max_dec_frame_buffering = 1;
};

struct Sps {
    uint8_t profile = 66, level = 21;
    uint8_t constraints = 0xC0;
    uint32_t id = 0;
    uint32_t log2_max_frame_num = 4;
    uint32_t poc_type = 2;
    uint32_t log2_max_poc_lsb = 6;                  // POC type 0
    bool delta_always_zero = false;                 // POC type 1
    int32_t offset_non_ref = 0, offset_top_bottom = 0;
    std::vector<int32_t> ref_frame_offsets = {2};   // POC type 1: the cycle
    uint32_t max_num_ref_frames = 1;
    bool gaps = false;
    uint32_t width_mbs = 4, height_map_units = 4;   // pic_width_in_mbs, pic_height_in_map_units
    bool frame_mbs_only = true, mbaff = false;
    bool direct_8x8 = true;
    bool crop = false;
    uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    Vui vui;
    uint32_t chroma_format_idc = 1; // High and above
};

inline void hrd(BitWriter& w)
{
    w.ue(0);      // cpb_cnt_minus1
    w.bits(0, 4); // bit_rate_scale
    w.bits(2, 4); // cpb_size_scale
    w.ue(1999);   // bit_rate_value_minus1
    w.ue(3999);   // cpb_size_value_minus1
    w.bit(0);     // cbr_flag
    w.bits(23, 5); // initial_cpb_removal_delay_length_minus1
    w.bits(23, 5); // cpb_removal_delay_length_minus1
    w.bits(23, 5); // dpb_output_delay_length_minus1
    w.bits(24, 5); // time_offset_length
}

inline void sps_data(BitWriter& w, const Sps& s, uint8_t profile)
{
    w.byte(profile);
    w.byte(s.constraints);
    w.byte(s.level);
    w.ue(s.id);
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128) {
        w.ue(s.chroma_format_idc);
        if (s.chroma_format_idc == 3) w.bit(0);
        w.ue(0);  // bit_depth_luma_minus8
        w.ue(0);  // bit_depth_chroma_minus8
        w.bit(0); // qpprime_y_zero_transform_bypass_flag
        w.bit(0); // seq_scaling_matrix_present_flag
    }
    w.ue(s.log2_max_frame_num - 4);
    w.ue(s.poc_type);
    if (s.poc_type == 0) {
        w.ue(s.log2_max_poc_lsb - 4);
    } else if (s.poc_type == 1) {
        w.bit(s.delta_always_zero ? 1 : 0);
        w.se(s.offset_non_ref);
        w.se(s.offset_top_bottom);
        w.ue(static_cast<uint32_t>(s.ref_frame_offsets.size()));
        for (const int32_t o : s.ref_frame_offsets) w.se(o);
    }
    w.ue(s.max_num_ref_frames);
    w.bit(s.gaps ? 1 : 0);
    w.ue(s.width_mbs - 1);
    w.ue(s.height_map_units - 1);
    w.bit(s.frame_mbs_only ? 1 : 0);
    if (!s.frame_mbs_only) w.bit(s.mbaff ? 1 : 0);
    w.bit(s.direct_8x8 ? 1 : 0);
    w.bit(s.crop ? 1 : 0);
    if (s.crop) {
        w.ue(s.crop_left);
        w.ue(s.crop_right);
        w.ue(s.crop_top);
        w.ue(s.crop_bottom);
    }
    const Vui& v = s.vui;
    w.bit(v.present ? 1 : 0);
    if (!v.present) return;
    w.bit(v.aspect_ratio_idc >= 0 ? 1 : 0);
    if (v.aspect_ratio_idc >= 0) {
        w.byte(static_cast<uint8_t>(v.aspect_ratio_idc));
        if (v.aspect_ratio_idc == 255) {
            w.bits(v.sar_width, 16);
            w.bits(v.sar_height, 16);
        }
    }
    w.bit(v.overscan >= 0 ? 1 : 0);
    if (v.overscan >= 0) w.bit(static_cast<uint32_t>(v.overscan));
    w.bit(v.video_signal ? 1 : 0);
    if (v.video_signal) {
        w.bits(v.video_format, 3);
        w.bit(v.full_range);
        w.bit(v.colour ? 1 : 0);
        if (v.colour) {
            w.byte(v.primaries);
            w.byte(v.transfer);
            w.byte(v.matrix);
        }
    }
    w.bit(v.chroma_loc ? 1 : 0);
    if (v.chroma_loc) {
        w.ue(v.chroma_top);
        w.ue(v.chroma_bottom);
    }
    w.bit(v.timing ? 1 : 0);
    if (v.timing) {
        w.bits(v.units_in_tick, 32);
        w.bits(v.time_scale, 32);
        w.bit(v.fixed_rate ? 1 : 0);
    }
    w.bit(v.nal_hrd ? 1 : 0);
    if (v.nal_hrd) hrd(w);
    w.bit(v.vcl_hrd ? 1 : 0);
    if (v.vcl_hrd) hrd(w);
    if (v.nal_hrd || v.vcl_hrd) w.bit(v.low_delay_hrd ? 1 : 0);
    w.bit(v.pic_struct ? 1 : 0);
    w.bit(v.restriction ? 1 : 0);
    if (v.restriction) {
        w.bit(1);  // motion_vectors_over_pic_boundaries_flag
        w.ue(0);   // max_bytes_per_pic_denom
        w.ue(0);   // max_bits_per_mb_denom
        w.ue(16);  // log2_max_mv_length_horizontal
        w.ue(16);  // log2_max_mv_length_vertical
        w.ue(v.num_reorder_frames);
        w.ue(v.max_dec_frame_buffering);
    }
}

inline std::vector<uint8_t> sps_rbsp(const Sps& s)
{
    BitWriter w;
    sps_data(w, s, s.profile);
    w.trailing();
    return w.take();
}

struct Pps {
    uint32_t id = 0, sps_id = 0;
    bool cabac = false;
    bool bottom_field_poc = false;
    uint32_t ref_l0 = 1, ref_l1 = 1; // num_ref_idx_lX_default_active
    bool deblocking_control = true;
    bool transform_8x8 = false; // High: the extension's fields are written when set
    int32_t qp = 26;
};

inline std::vector<uint8_t> pps_rbsp(const Pps& p)
{
    BitWriter w;
    w.ue(p.id);
    w.ue(p.sps_id);
    w.bit(p.cabac ? 1 : 0);
    w.bit(p.bottom_field_poc ? 1 : 0);
    w.ue(0); // num_slice_groups_minus1
    w.ue(p.ref_l0 - 1);
    w.ue(p.ref_l1 - 1);
    w.bit(0);     // weighted_pred_flag
    w.bits(0, 2); // weighted_bipred_idc
    w.se(p.qp - 26);
    w.se(0);
    w.se(0); // chroma_qp_index_offset
    w.bit(p.deblocking_control ? 1 : 0);
    w.bit(0); // constrained_intra_pred_flag
    w.bit(0); // redundant_pic_cnt_present_flag
    if (p.transform_8x8) {
        w.bit(1); // transform_8x8_mode_flag
        w.bit(0); // pic_scaling_matrix_present_flag
        w.se(0);  // second_chroma_qp_index_offset
    }
    w.trailing();
    return w.take();
}

// ---- pictures

enum class Kind { I, P, B, Bpcm };

// A memory management operation (dec_ref_pic_marking): its operation and its two possible values.
struct Mmco {
    uint32_t op = 0, a = 0, b = 0;
};

struct Slice {
    Kind kind = Kind::I;
    bool idr = false;
    uint32_t nal_ref_idc = 3;
    uint32_t frame_num = 0;
    uint32_t idr_pic_id = 0;
    uint32_t poc_lsb = 0;                       // POC type 0
    int32_t delta_poc_bottom = 0;               // POC type 0 with bottom_field_pic_order_in_frame_present
    int32_t delta_poc[2] = {0, 0};              // POC type 1 (when not delta_always_zero)
    bool field = false, bottom = false;         // field_pic_flag, bottom_field_flag
    bool no_output_of_prior_pics = false, long_term = false; // IDR
    std::vector<Mmco> mmco;                     // adaptive marking (non-IDR reference pictures)
    uint32_t pps_id = 0;
    uint32_t deblock_idc = 1;                   // disable_deblocking_filter_idc (1: off)
    bool direct_spatial = true;                 // B
    uint32_t first_mb = 0, mbs = 0;             // the slice's macroblocks (mbs 0: to the picture's end)
};

// The slice's header (7.3.3) as `s` and the active `sps`/`pps` have it.
inline void slice_header(BitWriter& w, const Sps& sps, const Pps& pps, const Slice& sl)
{
    w.ue(sl.first_mb);
    const uint32_t type = sl.kind == Kind::I ? 7 : sl.kind == Kind::P ? 5 : 6;
    w.ue(type);
    w.ue(sl.pps_id);
    w.bits(sl.frame_num, static_cast<int>(sps.log2_max_frame_num));
    if (!sps.frame_mbs_only) {
        w.bit(sl.field ? 1 : 0);
        if (sl.field) w.bit(sl.bottom ? 1 : 0);
    }
    if (sl.idr) w.ue(sl.idr_pic_id);
    if (sps.poc_type == 0) {
        w.bits(sl.poc_lsb, static_cast<int>(sps.log2_max_poc_lsb));
        if (pps.bottom_field_poc && !sl.field) w.se(sl.delta_poc_bottom);
    }
    if (sps.poc_type == 1 && !sps.delta_always_zero) {
        w.se(sl.delta_poc[0]);
        if (pps.bottom_field_poc && !sl.field) w.se(sl.delta_poc[1]);
    }
    if (sl.kind == Kind::B || sl.kind == Kind::Bpcm) w.bit(sl.direct_spatial ? 1 : 0);
    if (sl.kind != Kind::I) {
        w.bit(0); // num_ref_idx_active_override_flag
        w.bit(0); // ref_pic_list_modification_flag_l0
        if (sl.kind == Kind::B || sl.kind == Kind::Bpcm) w.bit(0); // _l1
    }
    if (sl.nal_ref_idc) {
        if (sl.idr) {
            w.bit(sl.no_output_of_prior_pics ? 1 : 0);
            w.bit(sl.long_term ? 1 : 0);
        } else {
            w.bit(sl.mmco.empty() ? 0 : 1);
            for (const Mmco& m : sl.mmco) {
                w.ue(m.op);
                if (m.op == 1 || m.op == 3) w.ue(m.a); // difference_of_pic_nums_minus1
                if (m.op == 2) w.ue(m.a);              // long_term_pic_num
                if (m.op == 3 || m.op == 6) w.ue(m.op == 3 ? m.b : m.a); // long_term_frame_idx
                if (m.op == 4) w.ue(m.a);              // max_long_term_frame_idx_plus1
            }
            if (!sl.mmco.empty()) w.ue(0);
        }
    }
    w.se(0); // slice_qp_delta
    if (pps.deblocking_control) {
        w.ue(sl.deblock_idc);
        if (sl.deblock_idc != 1) {
            w.se(0);
            w.se(0);
        }
    }
}

// One I_PCM macroblock of `p` at (mx, my) (frame macroblocks; for a field picture the field's lines).
inline void pcm_mb(BitWriter& w, const Picture& p, int mx, int my, bool field, bool bottom)
{
    w.align_zero();
    const int step = field ? 2 : 1, off = field && bottom ? 1 : 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) w.byte(p.y[((my * 16 + y) * step + off) * p.width + mx * 16 + x]);
    for (const std::vector<uint8_t>* plane : {&p.cb, &p.cr})
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) w.byte((*plane)[((my * 8 + y) * step + off) * (p.width / 2) + mx * 8 + x]);
}

// The slice's data: I and Bpcm pictures I_PCM macroblocks (mb_type 25 in I slices, 23 + 25 in B), P and B skip runs.
inline std::vector<uint8_t> slice_rbsp(const Sps& sps, const Pps& pps, const Slice& sl, const Picture& p)
{
    BitWriter w;
    slice_header(w, sps, pps, sl);
    const int mbw = static_cast<int>(sps.width_mbs);
    const int map_h = static_cast<int>(sps.height_map_units) * (sps.frame_mbs_only ? 1 : 2);
    const int mbh = sl.field ? map_h / 2 : map_h;
    const uint32_t total = static_cast<uint32_t>(mbw * mbh);
    const uint32_t count = sl.mbs ? sl.mbs : total - sl.first_mb;
    if (sl.kind == Kind::P || sl.kind == Kind::B) {
        w.ue(count); // mb_skip_run: every macroblock
    } else {
        for (uint32_t k = 0; k < count; k++) {
            const uint32_t mb = sl.first_mb + k;
            if (sl.kind == Kind::Bpcm) w.ue(23 + 25);
            else w.ue(25);
            pcm_mb(w, p, static_cast<int>(mb % static_cast<uint32_t>(mbw)), static_cast<int>(mb / static_cast<uint32_t>(mbw)),
                sl.field, sl.bottom);
        }
    }
    w.trailing();
    return w.take();
}

// ---- SEI

struct SeiMessage {
    uint32_t type = 0;
    std::vector<uint8_t> payload;
};

inline std::vector<uint8_t> sei_rbsp(const std::vector<SeiMessage>& msgs)
{
    BitWriter w;
    for (const SeiMessage& m : msgs) {
        uint32_t t = m.type;
        while (t >= 255) {
            w.byte(255);
            t -= 255;
        }
        w.byte(static_cast<uint8_t>(t));
        uint32_t n = static_cast<uint32_t>(m.payload.size());
        while (n >= 255) {
            w.byte(255);
            n -= 255;
        }
        w.byte(static_cast<uint8_t>(n));
        for (const uint8_t b : m.payload) w.byte(b);
    }
    w.trailing();
    return w.take();
}

// A payload written bit by bit, then byte-aligned as SEI payloads end (a 1 then 0s when not aligned).
inline std::vector<uint8_t> payload(const std::function<void(BitWriter&)>& body)
{
    BitWriter w;
    body(w);
    if (!w.aligned()) {
        w.bit(1);
        w.align_zero();
    }
    return w.take();
}

// user_data_registered_itu_t_t35 with ATSC A/53's closed captions ("GA94", cc_data): `pairs` CC byte pairs.
inline SeiMessage cc_data(const std::vector<std::array<uint8_t, 3>>& triplets)
{
    std::vector<uint8_t> p = {0xB5, 0x00, 0x31, 'G', 'A', '9', '4', 0x03};
    p.push_back(static_cast<uint8_t>(0x40 | (triplets.size() & 0x1F))); // process_cc_data_flag, cc_count
    p.push_back(0xFF);                                                    // em_data
    for (const auto& t : triplets) p.insert(p.end(), t.begin(), t.end());
    p.push_back(0xFF); // marker_bits
    return {4, p};
}

// ---- access units

// NAL unit types.
enum : uint8_t { NAL_SLICE = 1, NAL_IDR = 5, NAL_SEI = 6, NAL_SPS = 7, NAL_PPS = 8, NAL_AUD = 9, NAL_EOSEQ = 10,
    NAL_EOSTREAM = 11, NAL_FILLER = 12 };

inline std::vector<uint8_t> aud_rbsp(uint32_t primary_pic_type)
{
    BitWriter w;
    w.bits(primary_pic_type, 3);
    w.trailing();
    return w.take();
}

struct AuParts {
    bool aud = true, sps = false, pps = false;
    std::vector<SeiMessage> sei;
    bool end_of_seq = false, end_of_stream = false, filler = false;
};

// An access unit of one picture (or one field): [AUD] [SPS] [PPS] [SEI] slice(s) [filler] [end of sequence] [end of
// stream]. `slices` more than one cut the picture's macroblocks in as many slices.
inline std::vector<uint8_t> access_unit(const Sps& sps, const Pps& pps, Slice sl, const Picture& p, const AuParts& parts = {},
    uint32_t slices = 1)
{
    std::vector<uint8_t> out;
    if (parts.aud) put_nal(out, NAL_AUD, aud_rbsp(sl.kind == Kind::I ? 0 : sl.kind == Kind::P ? 1 : 2));
    if (parts.sps) put_nal(out, 0x60 | NAL_SPS, sps_rbsp(sps));
    if (parts.pps) put_nal(out, 0x60 | NAL_PPS, pps_rbsp(pps));
    if (!parts.sei.empty()) put_nal(out, NAL_SEI, sei_rbsp(parts.sei));
    const uint8_t header = static_cast<uint8_t>((sl.nal_ref_idc & 3) << 5 | (sl.idr ? NAL_IDR : NAL_SLICE));
    const int mbw = static_cast<int>(sps.width_mbs);
    const int map_h = static_cast<int>(sps.height_map_units) * (sps.frame_mbs_only ? 1 : 2);
    const uint32_t total = static_cast<uint32_t>(mbw * (sl.field ? map_h / 2 : map_h));
    const uint32_t per = (total + slices - 1) / slices;
    for (uint32_t k = 0; k < slices; k++) {
        Slice part = sl;
        part.first_mb = k * per;
        part.mbs = std::min(per, total - part.first_mb);
        put_nal(out, header, slice_rbsp(sps, pps, part, p));
    }
    if (parts.filler) {
        std::vector<uint8_t> f(16, 0xFF);
        f.push_back(0x80);
        put_nal(out, NAL_FILLER, f);
    }
    if (parts.end_of_seq) out.insert(out.end(), {0, 0, 0, 1, NAL_EOSEQ});
    if (parts.end_of_stream) out.insert(out.end(), {0, 0, 0, 1, NAL_EOSTREAM});
    return out;
}

// ---- MVC (Annex H): the second view of a stereo pair

// nal_unit_header_mvc_extension (H.7.3.1.1): its three bytes after the NAL unit's first.
struct MvcHeader {
    bool non_idr = true;
    uint32_t priority_id = 0, view_id = 1, temporal_id = 0;
    bool anchor = false, inter_view = true;
};

inline std::vector<uint8_t> mvc_header_bytes(const MvcHeader& h)
{
    const uint32_t v = (h.non_idr ? 1u : 0u) << 22 | (h.priority_id & 63) << 16 | (h.view_id & 1023) << 6 |
                       (h.temporal_id & 7) << 3 | (h.anchor ? 1u : 0u) << 2 | (h.inter_view ? 1u : 0u) << 1 | 1u;
    return {static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
}

// A NAL unit with the MVC extension header (types 14, 20).
inline void put_mvc_nal(std::vector<uint8_t>& out, uint8_t header, const MvcHeader& h, const std::vector<uint8_t>& rbsp)
{
    std::vector<uint8_t> payload = mvc_header_bytes(h);
    payload.insert(payload.end(), rbsp.begin(), rbsp.end());
    put_nal(out, header, payload);
}

// subset_seq_parameter_set_rbsp (H.7.3.2.1.4) for two views, 0 and 1: view 1 predicted from view 0 in anchor and other
// pictures; one level for both (Stereo High 128 or Multiview High 118).
inline std::vector<uint8_t> subset_sps_rbsp(const Sps& s, uint8_t profile = 128)
{
    BitWriter w;
    sps_data(w, s, profile);
    w.bit(1);  // bit_equal_to_one
    w.ue(1);   // num_views_minus1
    w.ue(0);   // view_id[0]
    w.ue(1);   // view_id[1]
    w.ue(1);   // num_anchor_refs_l0[1]
    w.ue(0);   //   anchor_ref_l0[1][0]
    w.ue(0);   // num_anchor_refs_l1[1]
    w.ue(1);   // num_non_anchor_refs_l0[1]
    w.ue(0);   //   non_anchor_ref_l0[1][0]
    w.ue(0);   // num_non_anchor_refs_l1[1]
    w.ue(0);   // num_level_values_signalled_minus1
    w.byte(s.level);
    w.ue(0);   // num_applicable_ops_minus1
    w.bits(0, 3); // applicable_op_temporal_id
    w.ue(1);   // applicable_op_num_target_views_minus1
    w.ue(0);
    w.ue(1);   // the target views
    w.ue(1);   // applicable_op_num_views_minus1
    w.bit(0);  // mvc_vui_parameters_present_flag
    w.bit(0);  // additional_extension2_flag
    w.trailing();
    return w.take();
}

enum : uint8_t { NAL_PREFIX = 14, NAL_SUBSET_SPS = 15, NAL_SLICE_EXT = 20 };

// The second view's part of an access unit: [subset SPS] [PPS] its slices (coded slice extensions, view 1). The
// picture is an anchor (inter-view prediction only) when `anchor`; an I picture of the second view is an IDR one when
// the base view's is (non_idr_flag 0).
inline std::vector<uint8_t> dependent_unit(const Sps& sps, const Pps& pps, Slice sl, const Picture& p, bool anchor,
    bool with_sps, bool with_pps, uint8_t profile = 128)
{
    std::vector<uint8_t> out;
    if (with_sps) put_nal(out, 0x60 | NAL_SUBSET_SPS, subset_sps_rbsp(sps, profile));
    if (with_pps) put_nal(out, 0x60 | NAL_PPS, pps_rbsp(pps));
    MvcHeader h;
    h.non_idr = !sl.idr;
    h.anchor = anchor;
    h.inter_view = sl.nal_ref_idc != 0;
    put_mvc_nal(out, static_cast<uint8_t>((sl.nal_ref_idc & 3) << 5 | NAL_SLICE_EXT), h, slice_rbsp(sps, pps, sl, p));
    return out;
}

// ---- the samples a decoder gives

// The rounded average of two pictures (B_Skip with spatial direct prediction and no motion).
inline Picture average(const Picture& a, const Picture& b)
{
    Picture out(a.width, a.height);
    for (size_t i = 0; i < a.y.size(); i++) out.y[i] = static_cast<uint8_t>((a.y[i] + b.y[i] + 1) >> 1);
    for (size_t i = 0; i < a.cb.size(); i++) {
        out.cb[i] = static_cast<uint8_t>((a.cb[i] + b.cb[i] + 1) >> 1);
        out.cr[i] = static_cast<uint8_t>((a.cr[i] + b.cr[i] + 1) >> 1);
    }
    return out;
}

} // namespace pspocket::hle::test::h264s
