#pragma once

// AT3+ files built in code for cellAtrac's tests (no game data, D-532): a RIFF WAVE header (fmt as
// WAVE_FORMAT_EXTENSIBLE with ATRAC3plus' GUID and configuration, fact, smpl, an unknown chunk, data) and frames that
// decode to silence. Shared by test_atrac.cpp and the parser fuzzing (test_fuzz_media.cpp, D-603).

#include <cstring>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::hle::test::atrac {

inline constexpr u32 kFrame = 0x2B0; // 48 kHz stereo, 128 kbps

// A valid silent stereo ATRAC3plus frame (as test_adec.cpp builds them).
inline std::vector<u8> silent_frame(u32 units)
{
    std::vector<u8> f(kFrame, 0);
    u32 bit = 0;
    const auto put = [&](u32 v, u32 n) {
        for (u32 i = 0; i < n; i++, bit++)
            if ((v >> (n - 1 - i)) & 1) f[bit / 8] |= static_cast<u8>(0x80 >> (bit % 8));
    };
    put(0, 1);
    put(1, 2);
    put(units - 1, 5);
    put(0, 1);
    for (u32 ch = 0; ch < 2; ch++) {
        put(0, 2);
        for (u32 q = 0; q < units; q++) put(0, 3);
    }
    put(0, 2);
    put(0, 2);
    put(0, 2);
    put(0, 1);
    put(0, 1);
    put(3, 2);
    return f;
}

struct FileSpec {
    u32 frames = 24;
    s32 samples = -1;     // the fact chunk's samples (-1: no fact)
    s32 offset = -1;      // the fact chunk's first sample offset (-1: a 4-byte fact)
    s32 loop_start = -1;  // smpl's loop (-1: none)
    s32 loop_end = -1;
    u32 rate = 48000;
    u16 tag = 0xFFFE;
    bool bad_guid = false;
    bool odd_chunk = false; // an unknown chunk of odd size before data
};

// An AT3+ file: RIFF WAVE, fmt (WAVE_FORMAT_EXTENSIBLE with ATRAC3plus' GUID and config: 48 kHz stereo, 0x2B0-byte
// frames), fact, smpl, an optional unknown chunk, data.
inline std::vector<u8> at3p_file(const FileSpec& s)
{
    std::vector<u8> f;
    const auto u16le = [&](u32 v) { f.push_back(static_cast<u8>(v)); f.push_back(static_cast<u8>(v >> 8)); };
    const auto u32le = [&](u32 v) { u16le(v & 0xFFFF); u16le(v >> 16); };
    const auto tag = [&](const char* t) { f.insert(f.end(), t, t + 4); };
    tag("RIFF");
    u32le(0);
    tag("WAVE");
    tag("fmt ");
    u32le(0x34);
    u16le(s.tag);
    u16le(2);
    u32le(s.rate);
    u32le(s.rate * kFrame / 2048);
    u16le(kFrame);
    u16le(0);
    u16le(0x22);
    u16le(0x800);
    u32le(3);
    static const u8 kGuid[16] = {0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44,
                                 0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62};
    f.insert(f.end(), kGuid, kGuid + 16);
    if (s.bad_guid) f[f.size() - 3] ^= 1;
    u16le(1);
    f.push_back(0x48); // 48 kHz (2 << 5), stereo (2 << 2)
    f.push_back(0x55); // 0x2B0 / 8 - 1
    for (u32 i = 0; i < 8; i++) f.push_back(0);
    if (s.samples >= 0) {
        tag("fact");
        u32le(s.offset >= 0 ? 8 : 4);
        u32le(static_cast<u32>(s.samples));
        if (s.offset >= 0) u32le(static_cast<u32>(s.offset));
    }
    if (s.loop_start >= 0) {
        tag("smpl");
        u32le(0x3C);
        for (u32 i = 0; i < 7; i++) u32le(0);
        u32le(1);
        u32le(0);
        u32le(0);
        u32le(0);
        u32le(static_cast<u32>(s.loop_start));
        u32le(static_cast<u32>(s.loop_end));
        u32le(0);
        u32le(0);
    }
    if (s.odd_chunk) {
        tag("LIST");
        u32le(5);
        for (u32 i = 0; i < 6; i++) f.push_back(0x20);
    }
    tag("data");
    u32le(s.frames * kFrame);
    for (u32 i = 0; i < s.frames; i++) {
        const auto fr = silent_frame(1 + (i * 7) % 28);
        f.insert(f.end(), fr.begin(), fr.end());
    }
    const u32 riff = static_cast<u32>(f.size() - 8);
    std::memcpy(f.data() + 4, &riff, 4);
    return f;
}

} // namespace pspocket::hle::test::atrac
