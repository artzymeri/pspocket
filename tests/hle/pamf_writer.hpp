#pragma once

// A synthetic MPEG program stream for the PAMF demultiplexer's tests (no game data, D-194): AVC video on channel 0 and
// ATRAC3plus audio on channel 0 in 2048-byte packs, a start code and a sync word split between two packs, a random
// access point named by a private stream 2, a timestamp with bit 31 set, a program end code. Shared by test_dmux.cpp
// and the parser fuzzing (test_fuzz_media.cpp, D-603). And a PAMF header for cellPamf's tests (D-193): two streams,
// AVC on channel 0 with three entry points and ATRAC3plus on channel 0 with one.

#include <catch_amalgamated.hpp>

#include <cstring>
#include <optional>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/core/types.hpp"

namespace pspocket::hle::test::pamf {

constexpr u32 kPack = 0x800;

struct Pes {
    u8 stream_id = 0;
    std::vector<u8> data; // after the PES header (private streams: from their private header)
    std::optional<u64> pts, dts;
    int rap_channel = -1; // a system header and a private stream 2 naming this video channel come first
};

inline void put_ts(std::vector<u8>& out, u8 prefix, u64 ts)
{
    out.push_back(static_cast<u8>(prefix << 4 | ((ts >> 29) & 0x0E) | 1));
    const u16 mid = static_cast<u16>(((ts >> 14) & 0xFFFE) | 1), low = static_cast<u16>(((ts << 1) & 0xFFFE) | 1);
    out.insert(out.end(), {static_cast<u8>(mid >> 8), static_cast<u8>(mid), static_cast<u8>(low >> 8), static_cast<u8>(low)});
}

inline void put_be16(std::vector<u8>& out, usize v) { out.insert(out.end(), {static_cast<u8>(v >> 8), static_cast<u8>(v)}); }

// One pack: header, maybe the system header and private stream 2, the PES packet, padding to 2048 bytes.
inline std::vector<u8> pack(const Pes& p)
{
    std::vector<u8> out = {0x00, 0x00, 0x01, 0xBA, 0x44, 0x00, 0x04, 0x00, 0x04, 0x01, 0x01, 0x89, 0xC3, 0xF8};
    if (p.rap_channel >= 0) {
        out.insert(out.end(), {0x00, 0x00, 0x01, 0xBB});
        put_be16(out, 12);
        out.insert(out.end(), {0x80, 0x04, 0xE1, 0x04, 0xE1, 0xFF, 0xE0, 0xE0, 0x00, 0xBD, 0xE0, 0x00});
        REQUIRE(out.size() == 32); // the next start code 16-byte aligned, as libdmuxpamf's SPU reads it
        out.insert(out.end(), {0x00, 0x00, 0x01, 0xBF});
        put_be16(out, 0x10);
        out.insert(out.end(), {0x00, static_cast<u8>(p.rap_channel)});
        out.insert(out.end(), 14, 0xFF);
    }
    const u8 flags = static_cast<u8>((p.pts ? 0x80 : 0) | (p.dts ? 0x40 : 0));
    const u8 header_data_length = static_cast<u8>((p.pts ? 5 : 0) + (p.dts ? 5 : 0));
    out.insert(out.end(), {0x00, 0x00, 0x01, p.stream_id});
    put_be16(out, 3 + header_data_length + p.data.size());
    out.insert(out.end(), {0x81, flags, header_data_length});
    if (p.pts) put_ts(out, p.dts ? 3 : 2, *p.pts);
    if (p.dts) put_ts(out, 1, *p.dts);
    out.insert(out.end(), p.data.begin(), p.data.end());
    REQUIRE(out.size() + 6 <= kPack);
    const usize padding = kPack - out.size() - 6;
    out.insert(out.end(), {0x00, 0x00, 0x01, 0xBE});
    put_be16(out, padding);
    out.insert(out.end(), padding, 0xFF);
    return out;
}

// Access unit bodies never hold a start code prefix (no 0 or 1 bytes) or an ATRAC3plus sync word (no 0x0F).
inline u8 body_byte(u32 seed, u32 i) { return static_cast<u8>(0x20 + (seed * 37 + i * 11) % 0xD0); }

// AVC: four access units, each from an access unit delimiter (00 00 01 09) on.
constexpr u32 kAvcSizes[4] = {900, 2500, 300, 700};
inline std::vector<u8> avc_es()
{
    std::vector<u8> es;
    for (u32 u = 0; u < 4; u++) {
        es.insert(es.end(), {0x00, 0x00, 0x01, 0x09, 0x10});
        for (u32 i = 5; i < kAvcSizes[u]; i++) es.push_back(body_byte(u, i));
    }
    return es;
}

// ATRAC3plus: five frames, each a sync word (0x0FD0), the size field ((n + 1) * 8 + 8 bytes with the 8-byte header),
// then the frame.
constexpr u32 kAt3Frame = (0x1F + 1) * 8 + 8, kAt3Frames = 5;
inline std::vector<u8> at3_es()
{
    std::vector<u8> es;
    for (u32 f = 0; f < kAt3Frames; f++) {
        es.insert(es.end(), {0x0F, 0xD0, 0x00, 0x1F});
        for (u32 i = 4; i < kAt3Frame; i++) es.push_back(body_byte(10 + f, i));
    }
    return es;
}

inline std::vector<u8> slice(const std::vector<u8>& v, usize from, usize to) { return {v.begin() + static_cast<std::ptrdiff_t>(from), v.begin() + static_cast<std::ptrdiff_t>(to)}; }

inline std::vector<u8> private_stream(u8 id, std::vector<u8> data)
{
    data.insert(data.begin(), {id, 0x00, 0x00, 0x00}); // the private header: no extra header bytes
    return data;
}

constexpr u64 kPtsHigh = 0x80003000; // bit 31 set: sign-extended from s32 on LLE

// The program stream: video cut inside the second unit, then between 00 00 and 01 09 of the third unit's delimiter;
// audio cut between 0x0F and 0xD0 of the fourth frame's sync word; a program end code last.
inline std::vector<u8> program_stream()
{
    const std::vector<u8> v = avc_es(), a = at3_es();
    const usize v_cut1 = 1500, v_cut2 = 900 + 2500 + 2, a_cut = 3 * kAt3Frame + 1;
    std::vector<u8> s;
    const auto add = [&](const Pes& p) {
        const std::vector<u8> pk = pack(p);
        s.insert(s.end(), pk.begin(), pk.end());
    };
    add({0xE0, slice(v, 0, v_cut1), 0x1000, 0x0F00, 0});
    add({0xBD, private_stream(0x00, slice(a, 0, a_cut)), 0x1000, std::nullopt});
    add({0xE0, slice(v, v_cut1, v_cut2), 0x2000, std::nullopt});
    add({0xBD, private_stream(0x00, slice(a, a_cut, a.size())), 0x5000, std::nullopt});
    add({0xE0, slice(v, v_cut2, v.size()), kPtsHigh, std::nullopt});
    std::vector<u8> end(kPack, 0xFF);
    end[0] = 0x00, end[1] = 0x00, end[2] = 0x01, end[3] = 0xB9;
    s.insert(s.end(), end.begin(), end.end());
    return s;
}


// The header: sequence info at 0x50, its grouping period at 0x70, the group at 0x82, two 48-byte stream headers at
// 0x88, the entry point tables right after them (0xE8, then 0x10C), as libpamf lays them out.
inline std::vector<u8> pamf_header()
{
    std::vector<u8> header(0x800, 0);
    u8* h = header.data();
    std::memcpy(h, "PAMF0041", 8);
    ps_st32(h + 0x08, 1);    // header size, in 2048-byte units
    ps_st32(h + 0x0C, 0x10); // data size
    const u32 seq = 0x50;
    ps_st32(h + seq, 0x94);                  // size, up to the end of the streams
    ps_st32(h + seq + 0x08, 0x1000);         // start pts
    ps_st32(h + seq + 0x0E, 0x100000);       // end pts
    ps_st32(h + seq + 0x12, 0xFF123456);     // mux rate bound (22 bits used)
    ps_st32(h + seq + 0x1A, 2);              // total streams
    ps_st8(h + seq + 0x1F, 1);               // grouping periods
    const u32 gp = 0x70;
    ps_st32(h + gp, 0x74);
    ps_st32(h + gp + 0x06, 0x1000);
    ps_st32(h + gp + 0x0C, 0x100000);
    ps_st8(h + gp + 0x11, 1); // groups
    const u32 group = 0x82;
    ps_st32(h + group, 0x62);
    ps_st8(h + group + 5, 2); // streams
    // Stream 0: AVC, channel 0, 3 entry points at 0xE8. High profile 4.1, 1280x720, frame-MBs-only, 29.97 fps
    // (frameRateInfo 3 + 1), SAR given, signal information (format 5, full range, BT.709), CABAC, deblocking.
    const u32 avc = 0x88;
    ps_st8(h + avc + 0, 0x1b);
    ps_st8(h + avc + 4, 0xe0);
    ps_st32(h + avc + 8, 0xE8);
    ps_st32(h + avc + 12, 3);
    const u8 avc_info[26] = {100, 41, 0xC4, 0xff, 0, 16, 0, 9, 0, 80, 0, 45, 0, 0, 0, 0, 0, 0, 0, 8, 0xB0, 1, 1, 1,
        0xE2, 0x30};
    std::memcpy(h + avc + 16, avc_info, sizeof avc_info);
    // Stream 1: ATRAC3plus, channel 0, 1 entry point at 0x10C; stereo, 48 kHz.
    const u32 at3 = 0xB8;
    ps_st8(h + at3 + 0, 0xdc);
    ps_st8(h + at3 + 4, 0xbd);
    ps_st8(h + at3 + 5, 0x00);
    ps_st32(h + at3 + 8, 0x10C);
    ps_st32(h + at3 + 12, 1);
    ps_st8(h + at3 + 16 + 2, 2);
    ps_st8(h + at3 + 16 + 3, 1);
    // Entry points: value0 (indexN - 1 in the top two bits, the reference picture offset), pts, rpn offset.
    const u32 eps[3][3] = {{(1u << 14) | 5, 0x1000, 0}, {(1u << 14) | 6, 0x5000, 4}, {(2u << 14) | 7, 0x9000, 8}};
    for (u32 i = 0; i < 3; i++) {
        ps_st16(h + 0xE8 + 12 * i, static_cast<u16>(eps[i][0]));
        ps_st32(h + 0xE8 + 12 * i + 4, eps[i][1]);
        ps_st32(h + 0xE8 + 12 * i + 8, eps[i][2]);
    }
    ps_st32(h + 0x10C + 4, 0x1000);
    return header;
}

} // namespace pspocket::hle::test::pamf
