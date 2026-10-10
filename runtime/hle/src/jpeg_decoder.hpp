#pragma once

// pspocket's JPEG decoder for cellJpgDec (D-200), written here to decode as Sony's libjpgdec does, sample for sample
// (measured against it, run 107). libjpgdec is IJG libjpeg 6b ("6b 27-Mar-1998") with Sony's changes, and these are
// what it runs on the PPU: libjpeg's float IDCT (jidctflt.c) in double precision for the quality method; for the fast
// method Sony's own 16-bit AltiVec IDCT in the tangent form; for 1/2 a float AltiVec 4-point IDCT and for 1/4 a float
// 2-point one, of the low coefficients; libjpeg's upsampling (fancy, or box and its merged upsampler) and its YCbCr to
// RGB tables, except that each row's first multiple of 16 (merged: 32) pixels go through Sony's AltiVec conversion;
// libjpeg's entropy decoding with a 64-bit bit buffer, its handling of damaged data, and Sony's DC sums for its mean.
// No libjpeg or Sony code is linked or copied: the algorithms are written from libjpeg's documented behaviour and the
// measurements.

#include <cstdint>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::hle::jpeg {

enum class ColorSpace { Unknown, Gray, YCbCr, RGB, CMYK, YCCK };

struct ComponentInfo {
    int id = 0, h = 1, v = 1, tq = 0; // sampling factors, quantisation table
    int td = 0, ta = 0;               // the last scan's Huffman tables
    u32 width_in_blocks = 0, height_in_blocks = 0;
    std::vector<s16> coefficients; // 64 per block, natural order, blocks row by row (padded to whole MCUs)
    u32 blocks_per_row = 0, block_rows = 0;
};

struct Header {
    u32 width = 0, height = 0;
    int components = 0;
    bool progressive = false;
    ColorSpace color_space = ColorSpace::Unknown;
    bool jfif = false, adobe = false;
    int adobe_transform = -1;
};

class Decoder {
public:
    // Reads the markers up to the first scan (jpeg_read_header). False with the reason.
    bool read_header(const u8* data, usize size, std::string& error);
    const Header& header() const { return header_; }

    // Decodes every scan into coefficients (after read_header). False with the reason for what pspocket cannot decode;
    // damaged data decodes as libjpeg decodes it, with warnings() counting libjpeg's warnings and ran_out() telling
    // that the file ended first (libjpgdec then stops, lines_written() lines into the output).
    bool decode(std::string& error);
    int warnings() const { return warnings_; }
    bool ran_out() const { return ran_out_; }
    u32 lines_written(int scale, bool fast, ColorSpace out_space, u32 height) const;

    // The image at 1/scale (1, 2, 4 or 8) in `out_space`: Gray (1 byte a pixel), RGB (3), or YCbCr (3, upsampled, not
    // converted). `fast`: the fast method (jidctfst.c, box upsampling). Rows of out_width * bytes.
    struct Output {
        u32 width = 0, height = 0, components = 0;
        std::vector<u8> pixels;
    };
    bool output(int scale, bool fast, ColorSpace out_space, Output& out, std::string& error) const;

    // libjpgdec's mean of the first component (CellJpgDecDataOutInfo.mean; see the .cpp), after decode.
    float mean() const;

    // libjpgdec's useMemorySpace for an output at 1/scale (see the .cpp), after read_header.
    u32 memory_estimate(int scale, bool fast, u32 output_width, u32 output_components) const;

    // For tests and probes: the components (with their coefficients) and the quantisation tables.
    const std::vector<ComponentInfo>& components() const { return comps_; }
    const u16* quant(int table) const { return quant_[table]; }

private:
    struct Huffman {
        bool present = false;
        bool bad = false; // its counts overflow a code length: refused when a scan uses it (JERR_BAD_HUFF_TABLE)
        u8 lookup_length[256]{};
        u8 lookup_value[256]{};
        int maxcode[18]{};
        int valoffset[17]{};
        std::vector<u8> values;
    };
    struct Scan;
    struct Input;
    struct Bits;
    bool parse_sof(const u8* p, u32 len, int marker, std::string& error);
    bool parse_dht(const u8* p, u32 len, std::string& error);
    bool parse_dqt(const u8* p, u32 len, std::string& error);
    bool decode_scan(const u8* p, u32 len, Input& in, std::string& error);

    const u8* data_ = nullptr;
    usize size_ = 0, pos_ = 0; // pos_: at the first SOS after read_header
    Header header_;
    std::vector<ComponentInfo> comps_;
    int max_h_ = 1, max_v_ = 1;
    u32 mcus_x_ = 0, mcus_y_ = 0;
    u16 quant_[4][64]{}; // natural order
    bool quant_present_[4]{};
    Huffman dc_[4], ac_[4];
    u32 restart_interval_ = 0;
    s64 dc_sum_[10]{}; // libjpgdec's DC sums per block of the MCU, over every scan
    int warnings_ = 0;            // libjpeg's warnings while decoding (damaged data)
    bool ran_out_ = false;        // the file ended before EOI
    bool multiple_scans_ = false; // libjpeg buffers the whole image before output
    u32 rows_done_ = 0;           // iMCU rows decoded before the data ran out (single-scan files)
    int scans_ = 0;
    u32 last_mcus_ = 0; // the last scan's MCU count
};

} // namespace pspocket::hle::jpeg
